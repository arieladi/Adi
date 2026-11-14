// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/mix/focus/Core.hpp"
#include "adi/mix/focus/Transform.hpp"
#include "adi/mix/focus/Mailbox.hpp"
#include "adi/mix/focus/Filters.hpp"
#include <atomic>
#include <thread>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>
#include <type_traits>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif
namespace f=adi::mix::focus;
namespace {
thread_local bool inAudio=false;
std::uint64_t allocations=0,deallocations=0;
int checks=0,failures=0;
std::uint64_t comparedSamples=0;
void check(bool result,const char *label){++checks;if(!result){++failures;std::printf("FAIL %s\n",label);}}
struct AudioScope {AudioScope(){inAudio=true;}~AudioScope(){inAudio=false;}};
template<class T> bool render(f::Core &core,std::vector<T> &left,std::vector<T> &right,
    const std::vector<T> &kl,const std::vector<T> &kr,const f::Settings &s,int keyChannels=2,int block=256) {
    bool okay=true;
    for(std::size_t start=0;start<left.size();start+=static_cast<std::size_t>(block)) {
        const auto length=std::min(static_cast<std::size_t>(block),left.size()-start);
        std::array<T*,2> main{left.data()+start,right.data()+start};
        std::array<const T*,2> key{kl.data()+start,kr.data()+start};
        AudioScope scope;
        const auto context=core.beginBlock(0,core.latencySamples(),s);
        okay=core.process(main.data(),key.data(),keyChannels,static_cast<std::int32_t>(length),s,context)&&okay;
    }
    return okay;
}
void fftChecks() {
    for(const std::size_t n:{8U,1024U,16384U}) {
        f::detail::Transform transform;transform.prepare(n);
        std::vector<double> x(n),y(n);std::vector<std::complex<double>> spectrum(n/2+1);
        for(std::size_t i=0;i<n;++i)x[i]=.2*std::sin(static_cast<double>(i)*.173)+.03*std::cos(static_cast<double>(i)*.43);
        {AudioScope scope;transform.forward(x,spectrum);transform.inverse(spectrum,y);}
        double maximum=0.;for(std::size_t i=0;i<n;++i)maximum=std::max(maximum,std::abs(x[i]-y[i]));
        check(maximum<2.e-14,"prepared double transform roundtrip");
    }
}
void mailboxConcurrency() {
    f::detail::ConfigurationMailbox mailbox;
    auto signature=[](std::uint64_t value) {
        f::Settings s;const double n=static_cast<double>(value%1000);
        s.nodes[0].frequencyHz=1000.+n;s.nodes[0].q=.1+n*.0049;
        s.nodes[9].frequencyHz=20000.-n;s.nodes[9].enabled=(value%1000)%2==0;
        s.outputGainDb=n/100.;s.matchAmount=n/1000.;return s;
    };
    mailbox.publish(signature(0));std::atomic<bool> done{false};
    std::atomic<std::uint64_t> rejected{0};
    std::thread producer([&]{for(std::uint64_t i=1;i<=100000;++i){AudioScope scope;if(mailbox.publish(signature(i))==0)rejected.fetch_add(1);}done.store(true,std::memory_order_release);});
    std::uint64_t reads=0,mixed=0,last=0;
    while(!done.load(std::memory_order_acquire)) {
        f::Settings snapshot;std::uint64_t version=0;
        if(!mailbox.read(snapshot,version))continue;
        ++reads;const auto n=static_cast<std::uint64_t>(snapshot.nodes[0].frequencyHz-1000.);
        if(!(snapshot==signature(n)))++mixed;
        if(version<last)++mixed;
        last=version;
    }
    producer.join();f::Settings final;std::uint64_t version=0;
    check(mailbox.read(final,version)&&final==signature(100000)&&version==100001,"mailbox latest complete snapshot converges");
    check(reads>0&&mixed==0&&rejected.load()==0,"concurrent mailbox fields and serials never mix");
}
void coreConcurrency() {
    f::Core core;f::Settings initial;initial.nodes[0].enabled=true;initial.nodes[0].q=1.;
    core.prepare({48000,64,2},initial);
    std::atomic<bool> done{false};std::atomic<int> bad{0};f::Settings finalDesired=initial;
    std::thread producer([&]{
        f::Settings requested=initial;
        std::array<std::array<double,64>,2> x{},key{};
        std::array<double*,2> main{x[0].data(),x[1].data()};
        std::array<const double*,2> keys{key[0].data(),key[1].data()};
        for(std::uint64_t block=1;block<=999;++block) {
            const double value=static_cast<double>(block%100);
            requested.nodes[0].frequencyHz=1000.+value;requested.nodes[0].q=1.+value*.01;
            requested.inputGainDb=value*.001;
            for(std::size_t c=0;c<2;++c)for(std::size_t i=0;i<64;++i) {
                const double t=static_cast<double>(block*64+i);
                x[c][i]=.1*std::sin(t*.13+static_cast<double>(c));key[c][i]=.2*std::sin(t*.17+static_cast<double>(c));
            }
            {AudioScope scope;const auto context=core.beginBlock(block,0,requested);
             if(!core.process(main.data(),keys.data(),2,64,requested,context))bad.fetch_add(1);}
            for(const auto &channel:x)for(double valueOut:channel)if(!std::isfinite(valueOut))bad.fetch_add(1);
        }
        finalDesired=requested;done.store(true,std::memory_order_release);
    });
    std::uint64_t services=0;
    while(!done.load(std::memory_order_acquire)){if(core.serviceConfiguration())++services;else std::this_thread::yield();}
    producer.join();
    for(std::uint64_t i=0;i<4;++i){core.serviceConfiguration();static_cast<void>(core.beginBlock(1001+i,0,finalDesired));}
    core.serviceConfiguration();static_cast<void>(core.beginBlock(1005,0,finalDesired));
    const auto status=core.configurationStatus();
    check(bad.load()==0&&services>0,"concurrent owner planning and audio processing stay valid");
    check(status.requested==status.ready&&status.ready==status.applied&&core.pendingLatencyChange().token==0,"black-box latest request drains, applies and clears its offer");
}
void defaultsAndReadiness() {
    f::Settings s;check(f::valid(s),"default settings valid");
    auto invalid=s;invalid.nodes[0].q=std::numeric_limits<double>::quiet_NaN();
    check(!f::valid(invalid),"NaN settings rejected");
    f::Core core;bool rejected=false;try{core.prepare({48000,256,1},s);}catch(const std::invalid_argument&){rejected=true;}
    check(rejected,"unsupported mono main rejected explicitly");core.prepare({48000,256,2},s);
    std::vector<double> l(2000),r(2000),k(2000,.1);
    for(std::size_t i=0;i<l.size();++i){l[i]=.1*std::sin(static_cast<double>(i)*.12);r[i]=.03*std::cos(static_cast<double>(i)*.17);}
    const auto originalL=l,originalR=r;
    check(render(core,l,r,k,k,s),"default processes valid split blocks");
    check(l==originalL&&r==originalR,"inactive default preserves doubles exactly");
    check(!core.requestConfiguration(invalid),"invalid request rejected");
    const auto initial=core.configurationStatus();
    auto a=s;a.engine=f::Engine::Fir;
    check(core.requestConfiguration(a)&&core.serviceConfiguration(),"owner prepares complete pending FIR offer");
    const auto offer=core.pendingLatencyChange();check(offer.token!=0&&offer.samples==12288,"FIR offer has exact prepared latency");
    auto b=a;b.phaseMode=f::PhaseMode::Rotation;b.rotationDegrees=90.;b.nodes[0].q=2.7;
    check(core.requestConfiguration(b),"latest request accepted behind offered generation");
    check(!core.serviceConfiguration(),"offered generation immutable until admission");
    check(core.pendingLatencyChange().token==offer.token&&core.pendingLatencyChange().samples==offer.samples,"later request cannot replace advertised latency");
    const auto old=core.beginBlock(100,0,b);
    check(core.latencySamples()==0,"wrong cached PDC retains old engine");
    const auto split=core.beginBlock(100,12288,b);
    check(split.generation==0&&core.latencySamples()==0,"wrong cached latency on same physical split is rejected");
    const auto admitted=core.beginBlock(101,12288,b);
    check(admitted.generation!=old.generation&&core.latencySamples()==12288,"new physical block admits exact ready generation");
    std::array<double*,2> main{l.data(),r.data()};std::array<const double*,2> key{k.data(),k.data()};
    {AudioScope scope;check(!core.process(main.data(),key.data(),2,16,b,old),"retired physical context rejected");}
    check(core.serviceConfiguration(),"latest request builds after first admission");
    const auto second=core.pendingLatencyChange();check(second.samples==13312&&second.token!=offer.token,"coalesced request owns next exact latency");
    static_cast<void>(core.beginBlock(102,13312,b));
    check(core.configurationStatus().applied==core.configurationStatus().ready,"ready and applied generations converge");
    auto c=b;
    for(int i=0;i<100;++i){c.nodes[0].frequencyHz=1000.+static_cast<double>(i);core.requestConfiguration(c);}
    check(core.serviceConfiguration(),"burst coalesces without dropping latest request");
    const auto ready=core.configurationStatus();check(ready.requested==ready.ready&&ready.ready>initial.ready,"last burst request becomes complete ready state");
    check(core.pendingLatencyChange().samples==13312,"equal-latency edit still creates stable generation");
    static_cast<void>(core.beginBlock(103,13312,c));
    check(core.configurationStatus().applied==ready.ready,"equal-latency coefficient generation admits physically");
    core.reset(s);check(core.latencySamples()==0&&core.statistics().processedFrames==0,"explicit owner reset clears stream state");
}
struct GeneratedAudio {
    std::array<std::vector<double>,2> main,key;
    explicit GeneratedAudio(std::size_t count) {
        for(std::size_t c=0;c<2;++c) {
            main[c].resize(count);key[c].resize(count);
            std::uint32_t noise=0x61c88647U+static_cast<std::uint32_t>(c)*0x19849131U;
            std::uint32_t keyNoise=0x7f4a7c15U+static_cast<std::uint32_t>(c)*0x31415927U;
            for(std::size_t i=0;i<count;++i) {
                noise=noise*1664525U+1013904223U;keyNoise=keyNoise*22695477U+1U;
                const double t=static_cast<double>(i),ch=static_cast<double>(c);
                main[c][i]=.13*std::sin(t*.173+ch)+.09*std::cos(t*.057+ch*.7)+
                    .07*(static_cast<double>(noise>>8U)/8388608.-1.);
                key[c][i]=.17*(static_cast<double>(keyNoise>>8U)/8388608.-1.)+
                    .12*std::sin(t*.281+ch*.4);
            }
        }
    }
    bool run(f::Core &core,const f::Settings &settings) {
        return render(core,main[0],main[1],key[0],key[1],settings);
    }
    GeneratedAudio range(std::size_t start,std::size_t count) const {
        GeneratedAudio result(0);
        for(std::size_t c=0;c<2;++c) {
            result.main[c].assign(main[c].begin()+static_cast<std::ptrdiff_t>(start),main[c].begin()+static_cast<std::ptrdiff_t>(start+count));
            result.key[c].assign(key[c].begin()+static_cast<std::ptrdiff_t>(start),key[c].begin()+static_cast<std::ptrdiff_t>(start+count));
        }
        return result;
    }
};
double compare(const GeneratedAudio &a,const GeneratedAudio &b,std::size_t first=0,std::size_t count=std::numeric_limits<std::size_t>::max()) {
    count=std::min(count,a.main[0].size()-first);double peak=0.;
    for(std::size_t c=0;c<2;++c)for(std::size_t i=first;i<first+count;++i) {
        peak=std::max(peak,std::abs(a.main[c][i]-b.main[c][i]));++comparedSamples;
    }
    return peak;
}
bool stage(f::Core &core,const f::Settings &settings,std::uint64_t physical,std::int32_t latency) {
    if(!core.requestConfiguration(settings)||!core.serviceConfiguration())return false;
    const auto offer=core.pendingLatencyChange();if(offer.token==0||offer.samples!=latency)return false;
    {AudioScope scope;static_cast<void>(core.beginBlock(physical,latency,settings));}
    const auto status=core.configurationStatus();
    return status.requested==status.ready&&status.ready==status.applied&&core.pendingLatencyChange().token==0;
}
f::Settings firSettings(double q=3.) {
    f::Settings s;s.engine=f::Engine::Fir;s.nodes[0].enabled=true;
    s.nodes[0].q=q;s.nodes[0].frequencyHz=1000.;s.nodes[0].thresholdDb=-60.;return s;
}
void coherentFirPolicy() {
    constexpr std::size_t hop=8192,count=65536;constexpr std::int32_t latency=12288;
    const GeneratedAudio signal(count);auto initial=firSettings(),final=initial;
    final.nodes[0].frequencyHz=2300.;final.nodes[0].q=2.1;
    f::Core frequencyFirst,qFirst,fresh;
    frequencyFirst.prepare({48000,256,2},initial);qFirst.prepare({48000,256,2},initial);fresh.prepare({48000,256,2},final);
    auto frequency=initial;frequency.nodes[0].frequencyHz=final.nodes[0].frequencyHz;
    auto q=initial;q.nodes[0].q=final.nodes[0].q;
    check(stage(frequencyFirst,frequency,1,latency)&&stage(frequencyFirst,final,2,latency),"frequency then Q admits coherent prepared generations");
    check(stage(qFirst,q,1,latency)&&stage(qFirst,final,2,latency),"Q then frequency admits coherent prepared generations");
    auto a=signal,b=signal,c=signal;
    check(a.run(frequencyFirst,final)&&b.run(qFirst,final)&&c.run(fresh,final),"equivalent FIR edit orders render generated signal");
    check(compare(a,b)==0.&&compare(a,c)==0.,"same final frequency and Q produce bit-identical fresh audio for either order");
    double reduction=0.;for(std::size_t i=static_cast<std::size_t>(latency);i<count;++i)
        reduction=std::max(reduction,std::abs(a.main[0][i]-signal.main[0][i-static_cast<std::size_t>(latency)]));
    check(reduction>1.e-3,"coherent order test observes active audio processing");
    frequencyFirst.reset(final);qFirst.reset(final);
    auto wide=final;wide.nodes[0].q=4.1;
    check(stage(frequencyFirst,wide,3,latency)&&stage(frequencyFirst,final,4,latency),"Q roundtrip prepares both extraction streams");
    a=signal;b=signal;check(a.run(frequencyFirst,final)&&b.run(qFirst,final)&&compare(a,b)==0.,"Q roundtrip before rendering recalls exact final sound");
    auto defaultQ=final;defaultQ.nodes[0].q=3.;auto narrow=defaultQ;narrow.nodes[0].q=1.;
    frequencyFirst.prepare({48000,256,2},defaultQ);qFirst.prepare({48000,256,2},narrow);
    check(stage(qFirst,defaultQ,5,latency),"explicit transition to advertised default Q admits");
    a=signal;b=signal;check(a.run(frequencyFirst,defaultQ)&&b.run(qFirst,defaultQ)&&compare(a,b)==0.,"fresh advertised Q3 equals a real change to Q3");
    fresh.prepare({48000,256,2},narrow);c=signal;check(c.run(fresh,narrow)&&compare(a,c)>1.e-4,"fresh advertised Q3 is observably distinct from Q1");
    frequencyFirst.reset(final);fresh.prepare({48000,256,2},final);a=signal;c=signal;
    check(a.run(frequencyFirst,final)&&c.run(fresh,final)&&compare(a,c)==0.,"explicit reset recalls the same fresh final FIR settings");
    qFirst.prepare({48000,256,2},final);b=signal;check(b.run(qFirst,final)&&compare(b,c)==0.,"owner reprepare recalls identical settings independently of earlier edits");
    auto iir=initial;iir.engine=f::Engine::Iir;frequencyFirst.prepare({48000,256,2},iir);
    auto first=iir;first.nodes[0].q=1.2;
    check(frequencyFirst.requestConfiguration(first)&&frequencyFirst.serviceConfiguration(),"coalescing begins with a complete IIR offer");
    check(frequencyFirst.requestConfiguration(final)&&!frequencyFirst.serviceConfiguration(),"latest FIR request coalesces behind the immutable IIR offer");
    static_cast<void>(frequencyFirst.beginBlock(10,0,final));
    check(frequencyFirst.serviceConfiguration(),"latency-separated FIR generation builds after IIR admission");
    const auto pending=frequencyFirst.pendingLatencyChange();
    const auto held=frequencyFirst.beginBlock(11,0,final);const auto split=frequencyFirst.beginBlock(11,latency,final);
    check(pending.samples==latency&&held.generation!=0&&split.generation==0&&frequencyFirst.latencySamples()==0,"latency-separated offer remains held throughout one physical callback");
    static_cast<void>(frequencyFirst.beginBlock(12,latency,final));
    a=signal;check(a.run(frequencyFirst,final)&&compare(a,c)==0.,"coalesced and latency-separated updates produce the same final generated output");
    // A running Q edit changes future hops without clearing an existing dry,
    // extracted-stream overlap or detector history. Compare the entire old-hop
    // interval first, then establish that later audio actually responds to Q.
    frequencyFirst.prepare({48000,256,2},initial);qFirst.prepare({48000,256,2},initial);
    constexpr std::size_t edit=hop+256;
    auto prefixA=signal.range(0,edit),prefixB=prefixA;
    check(prefixA.run(frequencyFirst,initial)&&prefixB.run(qFirst,initial)&&compare(prefixA,prefixB)==0.,"running policy test starts from identical nonzero histories");
    auto changed=initial;changed.nodes[0].q=4.7;
    check(stage(frequencyFirst,changed,20,latency),"running Q edit admits without resetting the stream");
    auto tailA=signal.range(edit,count-edit),tailB=tailA;
    check(tailA.run(frequencyFirst,changed)&&tailB.run(qFirst,initial),"running changed and unchanged FIR histories render");
    check(compare(tailA,tailB,0,hop-256)==0.,"already accumulated old-hop overlap and envelope state survive Q admission");
    check(compare(tailA,tailB,2*hop,hop)>1.e-5,"later FIR hops audibly respond to newly coherent Q");
    frequencyFirst.prepare({48000,256,2},initial);qFirst.prepare({48000,256,2},initial);
    prefixA=signal.range(0,edit);prefixB=prefixA;
    check(prefixA.run(frequencyFirst,initial)&&prefixB.run(qFirst,initial),"same-state recall test primes active histories");
    check(stage(frequencyFirst,changed,30,latency)&&stage(frequencyFirst,initial,31,latency),"running preset roundtrip coalesces prepared kernels without audio reset");
    tailA=signal.range(edit,count-edit);tailB=tailA;
    check(tailA.run(frequencyFirst,initial)&&tailB.run(qFirst,initial)&&compare(tailA,tailB)==0.,"returning settings before a new hop preserves uninterrupted audio history exactly");
}
void pinnedFrequencyCalibration() {
    for(const auto engine:{f::Engine::Iir,f::Engine::Fir}) {
        auto initial=firSettings(1.3);initial.engine=engine;f::Core changed,control;
        changed.prepare({48000,256,2},initial);control.prepare({48000,256,2},initial);
        const GeneratedAudio signal(32768);auto prefix=signal.range(0,8448),same=prefix;
        check(prefix.run(changed,initial)&&same.run(control,initial),"pending-frequency test primes running detector state");
        auto requested=initial;requested.nodes[0].frequencyHz=7100.;
        check(changed.requestConfiguration(requested),"frequency request publishes before owner planning");
        auto a=signal.range(8448,4096),b=a;
        check(a.run(changed,requested)&&b.run(control,initial)&&compare(a,b)==0.,"frequency-dependent threshold calibration remains pinned to admitted coefficients");
    }
}
void noStaleReactivation() {
    constexpr std::size_t gap=32768,tail=32768;
    GeneratedAudio impulse(256),quiet(gap),after(tail);
    for(std::size_t c=0;c<2;++c) {
        std::fill(impulse.main[c].begin(),impulse.main[c].end(),0.);
        std::fill(impulse.key[c].begin(),impulse.key[c].end(),0.);
        std::fill(quiet.main[c].begin(),quiet.main[c].end(),0.);
        std::fill(quiet.key[c].begin(),quiet.key[c].end(),0.);
        std::fill(after.main[c].begin(),after.main[c].end(),0.);
        std::fill(after.key[c].begin(),after.key[c].end(),0.);
    }
    impulse.main[0][255]=.5;impulse.main[1][255]=-.2;
    auto peak=[](const GeneratedAudio &audio){double maximum=0.;for(const auto &channel:audio.main)for(double value:channel)maximum=std::max(maximum,std::abs(value));return maximum;};
    f::Settings fir;fir.engine=f::Engine::Fir;f::Settings iir;
    f::Core core;core.prepare({48000,256,2},fir);auto in=impulse;
    check(in.run(core,fir)&&stage(core,iir,1,0),"FIR impulse enters delay before switching to IIR");
    auto gapAudio=quiet;check(gapAudio.run(core,iir)&&stage(core,fir,2,12288),"owner reactivation clears dormant FIR media before offering it");
    auto result=after;check(result.run(core,fir)&&peak(result)==0.,"elapsed FIR dry/overlap audio cannot reappear after an inactive gap");
    auto phase=iir;phase.phaseMode=f::PhaseMode::Rotation;phase.rotationDegrees=90.;
    core.prepare({48000,256,2},phase);in=impulse;
    check(in.run(core,phase)&&stage(core,iir,3,0),"phase impulse enters overlap before deactivation");
    gapAudio=quiet;check(gapAudio.run(core,iir)&&stage(core,phase,4,1024),"owner prepares clean phase reactivation while phase is dormant");
    result=after;check(result.run(core,phase)&&peak(result)==0.,"elapsed phase overlap and aligned-dry audio cannot resurrect");
    auto dynamic=firSettings(1.7);dynamic.engine=f::Engine::Iir;
    core.prepare({48000,256,2},dynamic);in=impulse;
    for(auto &channel:in.key)std::fill(channel.begin(),channel.end(),.3);
    check(in.run(core,dynamic)&&stage(core,fir,5,12288),"IIR impulse leaves nonzero filter history before dormant bank switch");
    gapAudio=quiet;check(gapAudio.run(core,fir)&&stage(core,dynamic,6,0),"dormant IIR filters and detector histories reset on reactivation");
    result=after;check(result.run(core,dynamic)&&peak(result)==0.,"inactive IIR history cannot emit old audio on reactivation");
}
void invalidCachedLatency() {
    f::Core core;f::Settings settings;core.prepare({48000,256,2},settings);
    const auto wrong=core.beginBlock(1,123,settings);
    check(wrong.generation==0,"unmatched active/ready graph latency rejects the context");
    std::array<double,2> left{.1,.2},right{.3,.4};const auto originalLeft=left,originalRight=right;
    std::array<double*,2> main{left.data(),right.data()};
    {AudioScope scope;check(!core.process(main.data(),nullptr,0,2,settings,wrong),"invalid cached latency cannot process samples");}
    check(left==originalLeft&&right==originalRight,"invalid context leaves samples unchanged");
}
void alternatingBankConcurrency() {
    f::Core core;f::Settings initial;core.prepare({48000,64,2},initial);
    std::atomic<bool> done{false};std::atomic<int> bad{0};
    std::thread audio([&]{
        std::array<std::array<double,64>,2> data{},key{};
        std::array<double*,2> main{data[0].data(),data[1].data()};
        std::array<const double*,2> keys{key[0].data(),key[1].data()};
        std::uint64_t physical=1,frame=0;
        auto renderBlock=[&](const f::Settings &desired,std::int32_t configured) {
            for(std::size_t c=0;c<2;++c)for(std::size_t i=0;i<64;++i) {
                const double t=static_cast<double>(frame+i)+static_cast<double>(c);
                data[c][i]=.1*std::sin(t*.12);key[c][i]=.2*std::cos(t*.17);
            }
            {AudioScope scope;const auto context=core.beginBlock(physical++,configured,desired);
             if(!core.process(main.data(),keys.data(),2,64,desired,context))bad.fetch_add(1);}
            for(const auto &channel:data)for(double value:channel)if(!std::isfinite(value))bad.fetch_add(1);
            frame+=64;
        };
        for(std::uint64_t round=1;round<=12;++round) {
            auto desired=initial;desired.engine=round%2==0?f::Engine::Iir:f::Engine::Fir;
            desired.phaseMode=round%3==0?f::PhaseMode::Rotation:f::PhaseMode::Off;
            desired.rotationDegrees=90.;desired.nodes[0].enabled=true;
            desired.nodes[0].frequencyHz=1000.+static_cast<double>(round)*17.;desired.nodes[0].q=1.+static_cast<double>(round)*.1;
            core.requestConfiguration(desired);std::size_t retries=0;
            while(core.pendingLatencyChange().token==0&&retries++<200000) {
                renderBlock(desired,core.latencySamples());std::this_thread::yield();
            }
            const auto offer=core.pendingLatencyChange();if(offer.token==0){bad.fetch_add(1);break;}
            const auto invalid=core.beginBlock(physical++,777777,desired);if(invalid.generation!=0)bad.fetch_add(1);
            renderBlock(desired,offer.samples);
            if(core.pendingLatencyChange().token!=0)bad.fetch_add(1);
            for(int i=0;i<3;++i)renderBlock(desired,core.latencySamples());
        }
        done.store(true,std::memory_order_release);
    });
    while(!done.load(std::memory_order_acquire)){core.serviceConfiguration();std::this_thread::yield();}
    audio.join();check(bad.load()==0,"alternating owner/audio banks and phase preserve readiness and reject wrong PDC");
    check(core.pendingLatencyChange().token==0,"alternating concurrent offers leave no orphan token");
}
void contextOwnership() {
    f::Settings settings,changed=settings;changed.nodes[0].q=1.7;
    f::Core first,second;first.prepare({48000,256,2},settings);
    static_cast<void>(first.beginBlock(1,0,settings));
    check(stage(first,changed,2,0),"first instance admits a local structural generation");
    const auto firstContext=first.beginBlock(2,0,changed);
    second.prepare({48000,256,2},settings);static_cast<void>(second.beginBlock(1,0,settings));
    check(stage(second,changed,2,0),"later second instance reaches the same local generation and serial");
    const auto secondContext=second.beginBlock(2,0,changed);
    check(firstContext.generation==secondContext.generation&&firstContext.block==secondContext.block&&firstContext.epoch!=secondContext.epoch,"prepare epoch distinguishes otherwise identical local context fields");
    std::array<float,2> lf{.1f,.2f},rf{.3f,.4f};std::array<float*,2> floats{lf.data(),rf.data()};
    std::array<double,2> ld{.1,.2},rd{.3,.4};std::array<double*,2> doubles{ld.data(),rd.data()};
    {AudioScope scope;
     check(!first.process(floats.data(),nullptr,0,2,changed,secondContext)&&!second.process(floats.data(),nullptr,0,2,changed,firstContext),"foreign contexts rejected for float processing");
     check(!first.process(doubles.data(),nullptr,0,2,changed,secondContext)&&!second.process(doubles.data(),nullptr,0,2,changed,firstContext),"foreign contexts rejected for double processing");}
    first.reset(settings);const auto reset=first.beginBlock(1,0,settings);
    check(reset.epoch!=firstContext.epoch,"owner reset creates a distinct preparation epoch");
    auto stale=reset;stale.epoch=firstContext.epoch;
    {AudioScope scope;check(!first.process(doubles.data(),nullptr,0,2,settings,stale),"reset-expired epoch cannot process even with current local fields");}
    first.prepare({48000,256,2},settings);const auto prepared=first.beginBlock(1,0,settings);
    stale=prepared;stale.epoch=reset.epoch;
    {AudioScope scope;check(!first.process(floats.data(),nullptr,0,2,settings,stale),"reprepare-expired epoch rejected for float processing");}
    alignas(f::Core) std::array<std::byte,sizeof(f::Core)> storage{};
    auto *object=std::construct_at(reinterpret_cast<f::Core*>(storage.data()));
    object->prepare({48000,256,2},settings);const auto oldAddressContext=object->beginBlock(1,0,settings);std::destroy_at(object);
    object=std::construct_at(reinterpret_cast<f::Core*>(storage.data()));
    object->prepare({48000,256,2},settings);const auto reused=object->beginBlock(1,0,settings);
    check(reused.generation==oldAddressContext.generation&&reused.block==oldAddressContext.block&&reused.epoch!=oldAddressContext.epoch,"reused object address cannot reuse a preparation epoch");
    {AudioScope scope;check(!object->process(doubles.data(),nullptr,0,2,settings,oldAddressContext),"destroyed object's context rejected at the same allocator address");}
    std::destroy_at(object);
}
template<class Sample> void heldCompositionOffer() {
    auto initial=firSettings(1.3);initial.engine=f::Engine::Iir;
    f::Core subject,control;subject.prepare({48000,64,2},initial);control.prepare({48000,64,2},initial);
    const auto original=subject.beginBlock(1,0,initial);
    auto desired=initial;desired.nodes[0].frequencyHz=7100.;desired.nodes[0].q=2.7;
    check(subject.requestConfiguration(desired)&&subject.serviceConfiguration(),"composition hold prepares an equal-latency coefficient offer");
    const auto offer=subject.pendingLatencyChange();const auto before=subject.configurationStatus();
    std::array<std::array<Sample,64>,2> a{},b{},keys{};
    std::array<Sample*,2> mainA{a[0].data(),a[1].data()},mainB{b[0].data(),b[1].data()};
    std::array<const Sample*,2> key{keys[0].data(),keys[1].data()};
    auto controls=initial;controls.inputGainDb=6.;desired.inputGainDb=6.;
    bool exact=true,freshContexts=true,validAudio=true;
    f::BlockContext previous=original,lastHeld;
    for(std::uint64_t block=2;block<=5;++block) {
        for(std::size_t c=0;c<2;++c)for(std::size_t i=0;i<64;++i) {
            const double t=static_cast<double>(block*64+i),channel=static_cast<double>(c);
            a[c][i]=static_cast<Sample>(.1*std::sin(.13*t+channel));
            keys[c][i]=static_cast<Sample>(.23*std::cos(.17*t+channel*.7));
        }
        b=a;
        {AudioScope scope;
         const auto held=subject.beginBlock(block,0,desired,false);
         const auto context=control.beginBlock(block,0,controls);
         freshContexts=freshContexts&&held.generation==original.generation&&held.block!=previous.block&&held.epoch==original.epoch;
         validAudio=subject.process(mainA.data(),key.data(),2,64,desired,held)&&validAudio;
         validAudio=control.process(mainB.data(),key.data(),2,64,controls,context)&&validAudio;
         previous=held;lastHeld=held;}
        for(std::size_t c=0;c<2;++c)for(std::size_t i=0;i<64;++i) {
            exact=exact&&a[c][i]==b[c][i];++comparedSamples;
        }
    }
    check(validAudio&&freshContexts,"false admission flag retains coefficients but starts valid new physical contexts");
    check(exact,"held prepared frequency/Q keeps old sound while current scalar gain applies immediately");
    check(subject.pendingLatencyChange().token==offer.token&&subject.configurationStatus().applied==before.applied,"parent hold cannot consume or replace equal-latency offer");
    const auto samePhysical=subject.beginBlock(5,0,desired,true);
    check(samePhysical.generation==lastHeld.generation&&subject.pendingLatencyChange().token==offer.token,"enabling admission later within one physical block still cannot switch");
    const auto admitted=subject.beginBlock(6,0,desired,true);
    check(admitted.generation!=lastHeld.generation&&subject.pendingLatencyChange().token==0,"next physical block explicitly admits the held offer");
    {AudioScope scope;check(!subject.process(mainA.data(),key.data(),2,64,desired,lastHeld),"context from held prior generation expires after aggregate admission");}
    auto fir=desired;fir.engine=f::Engine::Fir;
    check(subject.requestConfiguration(fir)&&subject.serviceConfiguration(),"composition prepares a different-latency offer after equal-latency admission");
    const auto next=subject.pendingLatencyChange();
    const auto wrong=subject.beginBlock(7,next.samples,fir,false);
    check(wrong.generation==0&&subject.pendingLatencyChange().token==next.token,"hold flag cannot accept a ready latency before its admission");
    const auto keep=subject.beginBlock(8,0,fir,false);
    check(keep.generation==admitted.generation&&keep.block!=admitted.block&&subject.latencySamples()==0,"different-latency hold continues only under active cached PDC");
    const auto badSame=subject.beginBlock(8,next.samples,fir,true);
    check(badSame.generation==0&&subject.pendingLatencyChange().token==next.token,"wrong cached latency on a held physical block is rejected without consuming offer");
    const auto final=subject.beginBlock(9,next.samples,fir,true);
    check(final.generation!=keep.generation&&subject.latencySamples()==next.samples&&subject.pendingLatencyChange().token==0,"complete aggregate can admit the different-latency offer next callback");
}
void immutableOfferConsumption() {
    f::Core core;f::Settings desired;core.prepare({48000,64,2},desired);
    auto context=core.beginBlock(1,0,desired);bool held=true,consumed=true;
    std::uint64_t physical=2;
    for(std::uint64_t i=1;i<=1000;++i) {
        desired.nodes[0].frequencyHz=1000.+static_cast<double>(i%99);
        desired.nodes[0].q=1.+static_cast<double>(i%17)*.1;
        if(!core.requestConfiguration(desired)||!core.serviceConfiguration()){consumed=false;break;}
        const auto offer=core.pendingLatencyChange();const auto before=core.configurationStatus();
        f::BlockContext first,second,admitted;
        {AudioScope scope;
         first=core.beginBlock(physical++,0,desired,false);
         second=core.beginBlock(physical++,0,desired,false);}
        held=held&&offer.token!=0&&first.generation==context.generation&&second.generation==context.generation&&
            first.block!=second.block&&core.pendingLatencyChange().token==offer.token&&core.configurationStatus().applied==before.applied;
        {AudioScope scope;admitted=core.beginBlock(physical++,offer.samples,desired,true);}
        consumed=consumed&&admitted.generation!=context.generation&&core.pendingLatencyChange().token==0&&
            core.configurationStatus().applied==offer.token;
        context=admitted;
    }
    check(held,"every immutable ready offer remains unconsumed across false-admission physical contexts");
    check(consumed,"every final-token ready offer consumes on true admission under valid cached PDC");
}
void configurationIdentityHelpers() {
    f::Settings a,b=a;
    check(f::sameConfiguration(a,b),"identical prepared configurations compare equal");
    b.nodes[9].frequencyHz+=1.;check(!f::sameConfiguration(a,b),"last node frequency participates in prepared identity");
    b=a;b.nodes[9].q+=.1;check(!f::sameConfiguration(a,b),"last node Q participates in prepared identity");
    b=a;b.engine=f::Engine::Fir;check(!f::sameConfiguration(a,b),"filter bank participates in prepared identity");
    struct PhaseCase {f::PhaseMode mode;double amount,angle;int group;};
    const std::array<PhaseCase,9> cases{{
        {f::PhaseMode::Off,.7,90.,0},{f::PhaseMode::Match,0.,0.,1},
        {f::PhaseMode::Match,.2,0.,2},{f::PhaseMode::Match,.8,0.,2},
        {f::PhaseMode::Rotation,0.,0.,3},{f::PhaseMode::Rotation,0.,180.,4},
        {f::PhaseMode::Rotation,0.,-180.,4},{f::PhaseMode::Rotation,0.,90.,5},
        {f::PhaseMode::Rotation,0.,-45.,5}}};
    for(std::size_t i=0;i<cases.size();++i)for(std::size_t j=0;j<cases.size();++j) {
        auto plan=a,controls=a;plan.phaseMode=cases[i].mode;plan.matchAmount=cases[i].amount;plan.rotationDegrees=cases[i].angle;
        controls.phaseMode=cases[j].mode;controls.matchAmount=cases[j].amount;controls.rotationDegrees=cases[j].angle;
        const bool equivalent=cases[i].group==cases[j].group;
        check(f::sameConfiguration(plan,controls)==equivalent,"phase identity distinguishes dry, inversion and transform while active values remain scalar");
        plan.engine=f::Engine::Fir;plan.nodes[0].frequencyHz=1200.;plan.nodes[0].q=1.3;
        controls.nodes[0].frequencyHz=7100.;controls.nodes[0].q=3.7;
        controls.inputGainDb=6.;controls.nodes[0].thresholdDb=-55.;controls.nodes[0].attackMs=19.;
        controls.nodes[0].enabled=true;controls.nodes[0].bypass=true;controls.nodes[0].solo=true;
        controls.phaseMinimumHz=300.;controls.phaseMaximumHz=18000.;
        const auto pinned=f::pinConfiguration(controls,plan);
        const double amount=equivalent?controls.matchAmount:plan.matchAmount;
        const double angle=equivalent?controls.rotationDegrees:plan.rotationDegrees;
        check(pinned.engine==plan.engine&&pinned.nodes[0].frequencyHz==1200.&&pinned.nodes[0].q==1.3&&
              pinned.phaseMode==plan.phaseMode&&pinned.matchAmount==amount&&pinned.rotationDegrees==angle&&
              pinned.inputGainDb==6.&&pinned.nodes[0].thresholdDb==-55.&&pinned.nodes[0].attackMs==19.&&
              pinned.nodes[0].enabled&&pinned.nodes[0].bypass&&pinned.nodes[0].solo&&
              pinned.phaseMinimumHz==300.&&pinned.phaseMaximumHz==18000.,
              "configuration pin retains exact prepared topology and latest independent scalar fields");
    }
}
void scalarOnlyRequests() {
    for(int profile=0;profile<4;++profile) {
        auto settings=firSettings(1.3);settings.engine=profile>=2?f::Engine::Fir:f::Engine::Iir;
        if(profile==1){settings.phaseMode=f::PhaseMode::Match;settings.matchAmount=.5;}
        if(profile==2){settings.phaseMode=f::PhaseMode::Rotation;settings.rotationDegrees=90.;}
        f::Core core;core.prepare({48000,256,2},settings);const auto baseline=core.configurationStatus();
        bool noWork=true;
        for(int step=0;step<22;++step) {
            switch(step) {
                case 0:settings.detector=f::Detector::Difference;break;
                case 1:settings.externalKey=false;break;
                case 2:settings.matchAmount=.8;break;
                case 3:settings.rotationDegrees=-45.;break;
                case 4:settings.phaseMinimumHz=300.;break;
                case 5:settings.phaseMaximumHz=18000.;break;
                case 6:settings.wet=.7;break;
                case 7:settings.inputGainDb=2.;break;
                case 8:settings.outputGainDb=-1.;break;
                case 9:settings.keyCalibrationDb=2.;break;
                case 10:settings.inputCalibrationDb=-2.;break;
                case 11:settings.nodes[0].thresholdDb=-55.;break;
                case 12:settings.nodes[0].differenceDb=-6.;break;
                case 13:settings.nodes[0].attackMs=19.;break;
                case 14:settings.nodes[0].releaseMs=130.;break;
                case 15:settings.nodes[0].position=.75;break;
                case 16:settings.nodes[0].bypass=true;break;
                case 17:settings.nodes[0].solo=true;break;
                case 18:settings.nodes[0].enabled=false;break;
                case 19:settings.nodes[0].enabled=true;break;
                case 20:settings.nodes[0].bypass=false;break;
                case 21:settings.nodes[0].solo=false;break;
            }
            {AudioScope scope;noWork=core.requestConfiguration(settings)&&noWork;}
            noWork=!core.serviceConfiguration()&&noWork;
            const auto state=core.configurationStatus();
            noWork=noWork&&state.requested==baseline.requested&&state.ready==baseline.ready&&
                state.applied==baseline.applied&&core.pendingLatencyChange().token==0;
        }
        check(noWork,"all scalar-only automation changes avoid owner generations and coefficient work");
        settings.nodes[0].q=2.1;
        check(stage(core,settings,1,core.latencySamples()),"next structural request captures the latest complete scalar values");
        f::Core fresh;fresh.prepare({48000,256,2},settings);const GeneratedAudio signal(32768);
        auto changed=signal,direct=signal;
        check(changed.run(core,settings)&&direct.run(fresh,settings)&&compare(changed,direct)==0.,"scalar cache followed by structural admission equals a fresh complete-settings stream");
    }
    f::Settings rotation;rotation.phaseMode=f::PhaseMode::Rotation;f::Core core;core.prepare({48000,64,2},rotation);
    auto inverse=rotation;inverse.rotationDegrees=180.;
    check(!f::sameConfiguration(rotation,inverse)&&stage(core,inverse,1,0),"zero-to-inversion changes topology even though both latencies are zero");
    auto active=inverse;active.rotationDegrees=90.;
    check(!f::sameConfiguration(inverse,active)&&stage(core,active,2,1024),"inversion-to-active rotation requires prepared latency admission");
    auto amount=active;amount.rotationDegrees=-45.;const auto before=core.configurationStatus();
    check(core.requestConfiguration(amount)&&!core.serviceConfiguration()&&core.configurationStatus().requested==before.requested,"interior active rotation angle remains a live scalar");
}
void pinnedCandidateWithMovingScalars() {
    auto plan=firSettings(1.3);plan.engine=f::Engine::Iir;plan.phaseMode=f::PhaseMode::Rotation;plan.rotationDegrees=90.;
    f::Core held,control;held.prepare({48000,64,2},plan);control.prepare({48000,64,2},plan);
    const auto initial=held.configurationStatus();bool exact=true,validAudio=true,noWork=true;
    std::array<std::array<double,64>,2>a{},b{},key{};
    for(std::uint64_t block=1;block<=64;++block) {
        auto latest=plan;latest.engine=f::Engine::Fir;latest.nodes[0].frequencyHz=7100.;latest.nodes[0].q=3.7;
        latest.inputGainDb=block%2==0?3.:-2.;latest.nodes[0].thresholdDb=block%2==0?-55.:-65.;
        latest.nodes[0].attackMs=block%2==0?19.:7.;latest.nodes[0].releaseMs=block%2==0?130.:45.;
        latest.nodes[0].position=block%2==0?.75:.25;latest.wet=block%2==0?.7:1.;
        latest.rotationDegrees=block%2==0?45.:-45.;latest.phaseMinimumHz=block%2==0?300.:0.;
        if(block%3==0){latest.phaseMode=f::PhaseMode::Match;latest.matchAmount=.8;}
        auto expected=plan;expected.inputGainDb=latest.inputGainDb;expected.nodes[0].thresholdDb=latest.nodes[0].thresholdDb;
        expected.nodes[0].attackMs=latest.nodes[0].attackMs;expected.nodes[0].releaseMs=latest.nodes[0].releaseMs;
        expected.nodes[0].position=latest.nodes[0].position;expected.wet=latest.wet;expected.phaseMinimumHz=latest.phaseMinimumHz;
        expected.rotationDegrees=block%3==0?plan.rotationDegrees:latest.rotationDegrees;
        const auto pinned=f::pinConfiguration(latest,plan);
        for(std::size_t c=0;c<2;++c)for(std::size_t i=0;i<64;++i) {
            const double t=static_cast<double>(block*64+i)+static_cast<double>(c);
            a[c][i]=.1*std::sin(.13*t)+.04*std::cos(.37*t);key[c][i]=.23*std::cos(.17*t);
        }
        b=a;
        for(const auto& range:{std::pair<std::size_t,std::int32_t>{0,17},{17,47}}) {
            std::array<double*,2>ma{a[0].data()+range.first,a[1].data()+range.first},mb{b[0].data()+range.first,b[1].data()+range.first};
            std::array<const double*,2>keys{key[0].data()+range.first,key[1].data()+range.first};
            {AudioScope scope;const auto ca=held.beginBlock(block,1024,pinned,false);const auto cb=control.beginBlock(block,1024,expected);
             validAudio=held.process(ma.data(),keys.data(),2,range.second,pinned,ca)&&validAudio;
             validAudio=control.process(mb.data(),keys.data(),2,range.second,expected,cb)&&validAudio;}
        }
        for(std::size_t c=0;c<2;++c)for(std::size_t i=0;i<64;++i){exact=exact&&a[c][i]==b[c][i];++comparedSamples;}
        noWork=!held.serviceConfiguration()&&noWork;
    }
    const auto final=held.configurationStatus();
    check(validAudio&&exact,"captured candidate keeps prepared frequency/bank/phase while moving scalar audio stays exact");
    check(noWork&&final.requested==initial.requested&&final.ready==initial.ready&&final.applied==initial.applied&&held.pendingLatencyChange().token==0,"pinning another candidate cannot coalesce an unrequested structural aggregate");
}

void stagedTransformBudget() {
    using C=std::complex<double>;
    for(const std::size_t size:{8U,8192U,16384U,32768U}) {
        f::detail::Transform staged,synchronous;staged.prepare(size);synchronous.prepare(size);
        std::vector<double> input(size),output(size);std::vector<C>a(size/2+1),b(size/2+1);
        for(std::size_t i=0;i<size;++i)input[i]=.13*std::sin(.173*static_cast<double>(i));
        synchronous.forward(input,a);staged.startForward(input,b);
        bool bounded=true;
        while(staged.busy()) {
            const auto before=staged.workUnits();
            bounded=staged.advance(3)<=3&&staged.workUnits()-before<=3&&bounded;
        }
        check(bounded&&a==b,"every staged forward primitive resumes with identical arithmetic");
        staged.startInverse(b,output);
        while(staged.busy()) {
            const auto before=staged.workUnits();
            bounded=staged.advance(1)==1&&staged.workUnits()-before==1&&bounded;
        }
        double error=0.;for(std::size_t i=0;i<size;++i)error=std::max(error,std::abs(input[i]-output[i]));
        check(bounded&&error<2.e-14,"inverse recombination/permutation/butterflies/normalization/unpack are charged");
        const auto before=synchronous.workUnits();synchronous.forward(input,a);
        check(synchronous.workUnits()-before>size,"a synchronous whole FFT exposes all work, not one stage");
        if(size==8) {
            bool dft=true;
            for(std::size_t k=0;k<=size/2;++k) {
                C direct{};
                for(std::size_t i=0;i<size;++i)direct+=input[i]*std::polar(1.,-2.*std::numbers::pi*
                    static_cast<double>(k*i)/static_cast<double>(size));
                dft=dft&&std::abs(direct-b[k])<2.e-15;
            }
            check(dft,"packed transform agrees with independent direct DFT");
        }
    }
}
void firSampleBudget() {
    for(const double rate:{44100.,48000.,88200.,96000.,176400.,192000.}) {
        auto settings=firSettings(1.3);for(auto &node:settings.nodes)node.enabled=true;
        f::Core core;core.prepare({rate,1<<20,2},settings);
        const auto initial=core.statistics();const auto p=static_cast<std::size_t>(core.latencySamples()/3);
        check(initial.firWorkQuota==373+22*(std::bit_width(p)-1),"prepared quota follows actual transform size");
        bool bounded=true,okay=true;std::array<double,2> main{},key{.1,-.13};
        std::array<double*,2>m{&main[0],&main[1]};std::array<const double*,2>k{&key[0],&key[1]};
        for(std::size_t sample=0;sample<3*p+1;++sample) {
            const auto before=core.statistics();
            {AudioScope scope;const auto context=core.beginBlock(0,core.latencySamples(),settings);
             okay=core.process(m.data(),k.data(),2,0,settings,context)&&okay;
             bounded=core.statistics().firWorkUnits==before.firWorkUnits&&bounded;
             main={.02,-.03};okay=core.process(m.data(),k.data(),2,1,settings,context)&&okay;}
            const auto after=core.statistics();
            bounded=after.firWorkUnits-before.firWorkUnits<=after.firWorkQuota&&bounded;
            bounded=after.firMaintenanceUnits-before.firMaintenanceUnits==f::Statistics::firMaintenancePerFrame&&bounded;
            if(sample+1==2*p)bounded=after.firJobsCompleted==1&&bounded;
            if(sample+1==3*p)bounded=after.firJobsCompleted==2&&bounded;
        }
        const auto final=core.statistics();
        check(okay&&bounded&&final.firMaxSampleWork<=final.firWorkQuota&&final.firDeadlineMisses==0,
              "ten-node FIR completes before every output deadline using only actual sample credit");
        check(final.phaseWorkUnits==0&&final.firJobsStarted==3&&final.firJobsCompleted>=2,
              "FIR job and independent phase counters have explicit units");
    }
}
void causalFirGenerations() {
    auto settings=firSettings(1.3);f::Core core;core.prepare({48000,8192,2},settings);
    const auto p=static_cast<std::size_t>(core.latencySamples()/3);
    std::vector<double> left(p+1,.03),right(p+1,-.02),key(p+1,.1);
    auto consume=[&](std::size_t count) {
        std::array<double*,2>m{left.data(),right.data()};std::array<const double*,2>k{key.data(),key.data()};
        AudioScope scope;return core.process(m.data(),k.data(),2,static_cast<std::int32_t>(count),settings,
            core.beginBlock(0,core.latencySamples(),settings));
    };
    const auto a=core.configurationStatus().applied;
    check(consume(1)&&core.statistics().firCaptureVersion==a,"first actual source sample freezes capture generation A");
    settings.nodes[0].frequencyHz=7100.;
    check(stage(core,settings,10,core.latencySamples()),"admit B midway through source capture A");
    const auto b=core.configurationStatus().applied;
    check(consume(p-1)&&core.statistics().firJobVersion==a,"B cannot rewrite any of the captured A block");
    check(consume(1)&&core.statistics().firCaptureVersion==b,"next source block starts under B");
    settings.nodes[0].q=2.1;settings.nodes[0].enabled=false;
    check(stage(core,settings,11,core.latencySamples()),"admit C while A executes and B is captured");
    const auto c=core.configurationStatus().applied;
    check(consume(p-1)&&core.statistics().firJobVersion==b&&core.statistics().firPreviousVersion==a,
          "job B and prior-spectrum A preserve independent immutable generations");
    check(consume(1)&&core.statistics().firCaptureVersion==c&&core.statistics().firCaptureMask==0&&
          core.statistics().firJobMask==1&&core.statistics().firPreviousMask==1,
          "source mask is frozen independently from later current enable controls");
    settings.nodes[0].frequencyHz=3900.;
    check(stage(core,settings,12,core.latencySamples()),"fourth active generation admits with three source tags held");
    settings.nodes[0].q=3.1;
    check(core.requestConfiguration(settings)&&core.serviceConfiguration(),"fifth slot builds while all four audio identities are held");
    const auto offer=core.pendingLatencyChange();
    settings.nodes[0].q=4.1;check(core.requestConfiguration(settings)&&!core.serviceConfiguration(),"sixth request coalesces behind immutable fifth offer");
    check(core.pendingLatencyChange().token==offer.token,"five-slot pressure cannot overwrite the retained offer");
    check(consume(p-1)&&core.statistics().firDeadlineMisses==0,"retained source generations drain without missing a deadline");
}
void firCallbackPartitions() {
    for(const double rate:{44100.,96000.,192000.}) {
        auto settings=firSettings(1.3);settings.nodes[1]=settings.nodes[0];settings.nodes[1].enabled=true;
        settings.nodes[1].frequencyHz=7200.;settings.nodes[1].position=.8;
        f::Core regular,split;regular.prepare({rate,65536,2},settings);split.prepare({rate,65536,2},settings);
        const auto count=static_cast<std::size_t>(regular.latencySamples())*2+257;
        const GeneratedAudio source(count);auto a=source,b=source;
        bool okay=a.run(regular,settings);
        const std::array<std::size_t,7> parts{1,3,17,63,256,1025,4093};
        std::size_t cursor=0,which=0;bool budget=true;
        while(cursor<count) {
            const auto length=std::min(parts[which++%parts.size()],count-cursor);
            std::array<double*,2>m{b.main[0].data()+cursor,b.main[1].data()+cursor};
            std::array<const double*,2>k{b.key[0].data()+cursor,b.key[1].data()+cursor};
            const auto before=split.statistics();
            {AudioScope scope;okay=split.process(m.data(),k.data(),2,static_cast<std::int32_t>(length),settings,
                split.beginBlock(0,split.latencySamples(),settings))&&okay;}
            const auto after=split.statistics();budget=after.firWorkUnits-before.firWorkUnits<=length*after.firWorkQuota&&budget;
            cursor+=length;
        }
        const auto x=regular.statistics(),y=split.statistics();
        check(okay&&budget&&compare(a,b)==0.&&x.firWorkUnits==y.firWorkUnits&&x.firJobsCompleted==y.firJobsCompleted&&
              x.firMaintenanceUnits==y.firMaintenanceUnits&&x.firMaintenanceUnits==count*f::Statistics::firMaintenancePerFrame,
              "sample credit/audio are identical under irregular callback splits at every size family");
    }
}


// Independent whole-kernel overlap-add oracle: no partition scheduling,
// generation slots or Core kernel/rate helpers are used here.
template<class Sample> void correctedRateOracle(double rate) {
    using C=std::complex<double>;
    const std::size_t h=rate<=48000.?8192:rate<=96000.?16384:32768,n=2*h,count=3*h+2048;
    auto settings=firSettings(1.3);settings.nodes[0].frequencyHz=rate>96000.?7200.:1000.;
    settings.nodes[0].attackMs=7.;settings.nodes[0].releaseMs=90.;
    settings.nodes[0].thresholdDb=-55.;settings.wet=.7;
    const auto &node=settings.nodes[0];
    const auto centre=static_cast<std::size_t>(std::floor(static_cast<double>(h)*node.frequencyHz/rate+.5));
    const auto width=2*static_cast<std::size_t>(std::floor(((.9/.1803)*static_cast<double>(centre)/node.q)/2.))+1;
    std::vector<double> window(width),time(n);std::vector<C> smallSpectrum(h/2+1,C{.001,0.}),kernel(n/2+1),spectrum(n/2+1),product(n/2+1);
    std::size_t peak=0;
    for(std::size_t i=0;i<width;++i) {
        const double angle=static_cast<double>(static_cast<float>(std::numbers::pi))*
            std::log10(.1+.9*static_cast<double>(i)/static_cast<double>(width));
        window[i]=.35875-.48829*std::cos(2.*angle)+.14128*std::cos(4.*angle)-.01168*std::cos(6.*angle);
        if(window[i]>window[peak])peak=i;
    }
    window[peak]=1.;
    const auto first=centre>peak?centre-peak:0,offset=peak>centre?peak-centre:0;
    if(first<h/2&&offset<width)for(std::size_t i=0;i<std::min(width-offset,h/2-first);++i)
        smallSpectrum[first+i]=.001+.999*window[offset+i];
    f::detail::Transform small,large;small.prepare(h);large.prepare(n);
    small.inverse(smallSpectrum,std::span<double>(time.data(),h));
    std::rotate(time.begin(),time.begin()+static_cast<std::ptrdiff_t>(h/2),time.begin()+static_cast<std::ptrdiff_t>(h));
    large.forward(time,kernel);
    const GeneratedAudio source(count);
    std::array<std::vector<Sample>,2> audio,key;
    std::array<std::vector<double>,4> input,band;
    for(auto &v:input)v.resize(count);
    for(auto &v:band)v.assign(count,0.);
    for(std::size_t c=0;c<2;++c) {
        audio[c].resize(count);key[c].resize(count);
        for(std::size_t i=0;i<count;++i) {
            audio[c][i]=static_cast<Sample>(source.main[c][i]);key[c][i]=static_cast<Sample>(source.key[c][i]);
        }
    }
    for(std::size_t i=0;i<count;++i) {
        input[0][i]=static_cast<double>(audio[0][i])+static_cast<double>(audio[1][i]);
        input[1][i]=static_cast<double>(audio[0][i])-static_cast<double>(audio[1][i]);
        input[2][i]=static_cast<double>(key[0][i])+static_cast<double>(key[1][i]);
        input[3][i]=static_cast<double>(key[0][i])-static_cast<double>(key[1][i]);
    }
    for(std::size_t stream=0;stream<4;++stream)for(std::size_t start=0;start+h<=count;start+=h) {
        std::copy_n(input[stream].data()+start,h,time.data());std::fill(time.begin()+static_cast<std::ptrdiff_t>(h),time.end(),0.);
        large.forward(time,spectrum);
        for(std::size_t i=0;i<=h;++i)product[i]=spectrum[i]*kernel[i];
        large.inverse(product,time);
        for(std::size_t i=0;i<n-1&&start+h+i<count;++i)band[stream][start+h+i]+=time[i];
    }
    f::detail::Envelope envelopes[2];
    const double threshold=std::min(0.,node.thresholdDb+17.56*std::exp(3.112e-5*node.frequencyHz)-16.77*std::exp(-.01135*node.frequencyHz));
    const double attack=std::exp(-1./(rate*.001*node.attackMs)),release=std::exp(-1./(rate*.001*node.releaseMs));
    std::array<std::vector<Sample>,2> expected;for(auto &v:expected)v.resize(count);
    for(std::size_t i=0;i<count;++i) {
        std::array<double,2> output{};
        for(std::size_t c=0;c<2;++c) {
            const double dry=i>=h*3/2?input[c][i-h*3/2]:0.;
            const double gain=envelopes[c].tick(band[c][i],band[c+2][i],threshold,attack,release,false,true);
            output[c]=dry-(1.-std::pow(10.,std::min(0.,gain)*.05))*band[c][i]*settings.wet;
        }
        expected[0][i]=static_cast<Sample>((output[0]+output[1])*.5);
        expected[1][i]=static_cast<Sample>((output[0]-output[1])*.5);
    }
    f::Core core;core.prepare({rate,256,2},settings);
    check(render(core,audio[0],audio[1],key[0],key[1],settings),"actual-rate FIR oracle stream renders");
    double maximum=0.,squares=0.;
    for(std::size_t c=0;c<2;++c)for(std::size_t i=0;i<count;++i) {
        const double error=static_cast<double>(audio[c][i])-static_cast<double>(expected[c][i]);
        maximum=std::max(maximum,std::abs(error));squares+=error*error;++comparedSamples;
    }
    std::printf("FIR_RATE_ORACLE rate=%.0f precision=%s max=%.17g rms=%.17g\n",rate,
        std::is_same_v<Sample,double>?"double":"float",maximum,std::sqrt(squares/static_cast<double>(2*count)));
    check(maximum<(std::is_same_v<Sample,double>?1.e-10:6.e-8),
          "complete output agrees with independent actual-rate whole-kernel and envelope equations");
}
void firRateOracles() {
    for(const double rate:{44100.,48000.,88200.,96000.,176400.,192000.}) {
        correctedRateOracle<double>(rate);correctedRateOracle<float>(rate);
    }
}


void phaseRateOracles() {
    using C=std::complex<double>;
    for(const double rate:{44100.,48000.,88200.,96000.,176400.,192000.})
    for(const auto mode:{f::PhaseMode::Rotation,f::PhaseMode::Match}) {
        const std::size_t n=rate<=48000.?1024:rate<=96000.?2048:4096,hop=n/4,count=5*n+19;
        f::Settings settings;settings.phaseMode=mode;settings.rotationDegrees=90.;settings.matchAmount=.7;settings.phaseMinimumHz=6000.;
        GeneratedAudio actual(count);
        const auto source=actual;
        std::array<std::vector<double>,2> mainRing,keyRing,out,expected;
        for(std::size_t c=0;c<2;++c) {
            mainRing[c].assign(n,0.);keyRing[c].assign(n,0.);out[c].assign(4*n,0.);expected[c].resize(count);
        }
        std::vector<double> window(n),time(n);std::vector<C>spectrum(n/2+1),keySpectrum(n/2+1);
        for(std::size_t i=0;i<n;++i) {
            const float angle=(static_cast<float>(i)*static_cast<float>(2.*std::numbers::pi))/static_cast<float>(n-1);
            window[i]=static_cast<double>(1.f-std::cos(angle))*.5;
        }
        f::detail::Transform transform;transform.prepare(n);
        const auto low=std::min(n/2,static_cast<std::size_t>(static_cast<double>(n-1)*6000./rate));
        for(std::size_t t=0;t<count;++t) {
            const std::array<double,2> x{source.main[0][t]+source.main[1][t],source.main[0][t]-source.main[1][t]};
            const std::array<double,2> k{source.key[0][t]+source.key[1][t],source.key[0][t]-source.key[1][t]};
            std::array<double,2> heard{};
            for(std::size_t c=0;c<2;++c) {
                heard[c]=out[c][t%(4*n)];out[c][t%(4*n)]=0.;
                mainRing[c][t%n]=x[c];keyRing[c][t%n]=k[c];
            }
            expected[0][t]=(heard[0]+heard[1])*.5;expected[1][t]=(heard[0]-heard[1])*.5;
            if((t+1)%hop==0)for(std::size_t c=0;c<2;++c) {
                for(std::size_t i=0;i<n;++i)time[i]=mainRing[c][(t+1+i)%n]*window[i];
                transform.forward(time,spectrum);
                if(mode==f::PhaseMode::Match) {
                    for(std::size_t i=0;i<n;++i)time[i]=keyRing[c][(t+1+i)%n]*window[i];
                    transform.forward(time,keySpectrum);
                }
                for(std::size_t i=0;i<n/2;++i) {
                    if((i==0&&low==0)||(i>0&&i<low))continue;
                    const auto z=spectrum[i];const auto angle=std::atan2(z.imag(),z.real());
                    double next=angle+std::numbers::pi*.5;
                    if(mode==f::PhaseMode::Match) {
                        const auto key=keySpectrum[i];
                        const auto keyAngle=key.real()==0.&&key.imag()==0.?0.:std::atan2(key.imag(),key.real());
                        next=.7*keyAngle+.3*angle;
                    }
                    spectrum[i]=std::polar(std::abs(z),next);
                    if(i==0)spectrum[i]={spectrum[i].real(),0.};
                }
                transform.inverse(spectrum,time);
                for(std::size_t i=0;i<n;++i)out[c][(t+1+i)%(4*n)]+=time[i]*window[i]/1.5;
            }
        }
        f::Core core;core.prepare({rate,256,2},settings);
        check(actual.run(core,settings),"phase minimum actual-rate oracle renders");
        double maximum=0.;
        for(std::size_t c=0;c<2;++c)for(std::size_t i=0;i<count;++i) {
            maximum=std::max(maximum,std::abs(actual.main[c][i]-expected[c][i]));++comparedSamples;
        }
        check(maximum<3.e-14&&core.statistics().phaseWorkUnits>0&&core.statistics().firWorkUnits==0,
              "phase cutoff uses actual sample rate with separately explicit atomic transform work");
    }
}


void sourceTaggedFirOracle() {
    using C=std::complex<double>;
    constexpr std::size_t h=8192,p=h/2,n=2*h,count=5*h;
    const std::array<double,3> qualities{1.3,3.7,.6};
    std::array<std::vector<C>,3> kernels;
    f::detail::Transform small,large;small.prepare(h);large.prepare(n);
    std::vector<double> time(n);std::vector<C>spectrum(h+1),product(h+1);
    for(std::size_t generation=0;generation<qualities.size();++generation) {
        const auto centre=static_cast<std::size_t>(std::floor(static_cast<double>(h)*1000./48000.+.5));
        const auto width=2*static_cast<std::size_t>(std::floor(((.9/.1803)*static_cast<double>(centre)/qualities[generation])/2.))+1;
        std::vector<double> window(width);std::size_t peak=0;
        for(std::size_t i=0;i<width;++i) {
            const double angle=static_cast<double>(static_cast<float>(std::numbers::pi))*
                std::log10(.1+.9*static_cast<double>(i)/static_cast<double>(width));
            window[i]=.35875-.48829*std::cos(2.*angle)+.14128*std::cos(4.*angle)-.01168*std::cos(6.*angle);
            if(window[i]>window[peak])peak=i;
        }
        window[peak]=1.;
        std::fill(spectrum.begin(),spectrum.end(),C{.001,0.});
        const auto first=centre>peak?centre-peak:0,offset=peak>centre?peak-centre:0;
        for(std::size_t i=0;first+i<h/2&&offset+i<width;++i)spectrum[first+i]=.001+.999*window[offset+i];
        small.inverse(std::span<const C>(spectrum.data(),h/2+1),std::span<double>(time.data(),h));
        std::rotate(time.begin(),time.begin()+static_cast<std::ptrdiff_t>(h/2),time.begin()+static_cast<std::ptrdiff_t>(h));
        std::fill(time.begin()+static_cast<std::ptrdiff_t>(h),time.end(),0.);
        kernels[generation].resize(h+1);large.forward(time,kernels[generation]);
    }
    const GeneratedAudio source(count);auto actual=source;
    std::array<std::vector<double>,4> input,band;
    for(auto&v:input)v.resize(count);for(auto&v:band)v.assign(count,0.);
    for(std::size_t i=0;i<count;++i) {
        input[0][i]=source.main[0][i]+source.main[1][i];input[1][i]=source.main[0][i]-source.main[1][i];
        input[2][i]=source.key[0][i]+source.key[1][i];input[3][i]=source.key[0][i]-source.key[1][i];
    }
    for(std::size_t stream=0;stream<4;++stream)for(std::size_t start=0;start+p<=count;start+=p) {
        std::copy_n(input[stream].data()+start,p,time.data());std::fill(time.begin()+static_cast<std::ptrdiff_t>(p),time.end(),0.);
        large.forward(time,spectrum);const auto&kernel=kernels[(start/p)%3];
        for(std::size_t i=0;i<=h;++i)product[i]=spectrum[i]*kernel[i];large.inverse(product,time);
        for(std::size_t i=0;i<p+h-1&&start+h+i<count;++i)band[stream][start+h+i]+=time[i];
    }
    auto settings=firSettings(qualities[0]);settings.nodes[0].thresholdDb=-55.;settings.nodes[0].attackMs=7.;settings.nodes[0].releaseMs=90.;
    f::Core core;core.prepare({48000,static_cast<std::int32_t>(p),2},settings);
    bool okay=true;
    for(std::size_t block=0;block<count/p;++block) {
        for(std::size_t half=0;half<2;++half) {
            if(half==1) {
                settings.nodes[0].q=qualities[(block+1)%3];
                okay=stage(core,settings,0,core.latencySamples())&&okay;
            }
            const auto start=block*p+half*(p/2);
            std::array<double*,2>m{actual.main[0].data()+start,actual.main[1].data()+start};
            std::array<const double*,2>k{actual.key[0].data()+start,actual.key[1].data()+start};
            AudioScope scope;okay=core.process(m.data(),k.data(),2,static_cast<std::int32_t>(p/2),settings,
                core.beginBlock(0,core.latencySamples(),settings))&&okay;
        }
    }
    f::detail::Envelope envelopes[2];double maximum=0.;
    const double threshold=std::min(0.,-55.+17.56*std::exp(3.112e-5*1000.)-16.77*std::exp(-.01135*1000.));
    const double attack=std::exp(-1./(48000.*.001*7.)),release=std::exp(-1./(48000.*.001*90.));
    for(std::size_t i=0;i<count;++i) {
        std::array<double,2>output{};
        for(std::size_t c=0;c<2;++c) {
            const double gain=envelopes[c].tick(band[c][i],band[c+2][i],threshold,attack,release,false,true);
            output[c]=(i>=h*3/2?input[c][i-h*3/2]:0.)-(1.-std::pow(10.,std::min(0.,gain)*.05))*band[c][i];
        }
        maximum=std::max(maximum,std::abs(actual.main[0][i]-(output[0]+output[1])*.5));
        maximum=std::max(maximum,std::abs(actual.main[1][i]-(output[0]-output[1])*.5));
        comparedSamples+=2;
    }
    std::printf("FIR_SOURCE_TAG_ORACLE max=%.17g\n",maximum);
    check(okay&&maximum<1.e-10,"captured A/B/C blocks retain their actual kernels through both arithmetic partitions");
}

void latencyAndSafety() {
    const std::size_t count=32768;
    std::vector<double> left(count,0.),right(count,0.),key(count,0.);
    left[0]=.5;right[0]=-.25;
    f::Settings s;s.engine=f::Engine::Fir;f::Core core;core.prepare({48000,256,2},s);
    check(render(core,left,right,key,key,s),"FIR dry-delay stream renders");
    check(left[12288]==.5&&right[12288]==-.25,"inactive FIR reports and produces exact dry delay");
    double other=0.;for(std::size_t i=0;i<count;++i)if(i!=12288)other=std::max(other,std::abs(left[i])+std::abs(right[i]));
    check(other==0.,"inactive FIR creates no spurious samples");
    s={};s.phaseMode=f::PhaseMode::Rotation;s.rotationDegrees=180.;core.prepare({48000,256,2},s);
    std::fill(left.begin(),left.end(),.2);std::fill(right.begin(),right.end(),-.3);
    check(render(core,left,right,key,key,s,0),"manual inversion works without external key");
    check(left[0]==-.2&&right[0]==.3&&core.latencySamples()==0,"endpoint rotation has zero latency");
    s.phaseMode=f::PhaseMode::Match;s.matchAmount=1.;core.prepare({48000,256,2},s);
    for(std::size_t i=0;i<count;++i){left[i]=.13*std::sin(.12*static_cast<double>(i));right[i]=.07*std::cos(.13*static_cast<double>(i));}
    const auto beforeL=left,beforeR=right;
    check(render(core,left,right,key,key,s),"connected silent key takes stable phase fallback");
    double error=0.;for(std::size_t i=2048;i<count;++i)error=std::max(error,std::max(std::abs(left[i]-beforeL[i-1024]),std::abs(right[i]-beforeR[i-1024])));
    check(error<1.e-14&&core.latencySamples()==1024,"silent Match retains PDC and fades to aligned dry");
    s={};s.detector=f::Detector::Difference;s.nodes[0].enabled=true;s.nodes[0].frequencyHz=1000.;s.nodes[0].q=1.;
    core.prepare({48000,256,2},s);
    for(std::size_t i=0;i<count;++i){left[i]=.1*std::sin(.13*static_cast<double>(i));right[i]=left[i];}
    const auto dry=left;check(render(core,left,right,key,key,s,0),"missing Difference key renders release path");
    error=0.;for(std::size_t i=8192;i<count;++i)error=std::max(error,std::abs(left[i]-dry[i]));
    check(error<1.e-12,"missing Difference key cannot demand unlimited cut");
}
}
void* operator new(std::size_t n){if(inAudio)++allocations;if(void*p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void operator delete(void*p)noexcept{if(inAudio&&p)++deallocations;std::free(p);}
void operator delete(void*p,std::size_t)noexcept{::operator delete(p);}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete[](void*p)noexcept{::operator delete(p);}
void operator delete[](void*p,std::size_t)noexcept{::operator delete(p);}
int main(){
    std::setvbuf(stdout,nullptr,_IONBF,0);
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE);_CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR,_CRTDBG_MODE_FILE);_CrtSetReportFile(_CRT_ERROR,_CRTDBG_FILE_STDERR);
    _set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
#endif
    try {sourceTaggedFirOracle();phaseRateOracles();firRateOracles();stagedTransformBudget();firSampleBudget();causalFirGenerations();firCallbackPartitions();fftChecks();mailboxConcurrency();defaultsAndReadiness();coreConcurrency();coherentFirPolicy();pinnedFrequencyCalibration();invalidCachedLatency();noStaleReactivation();alternatingBankConcurrency();contextOwnership();heldCompositionOffer<float>();heldCompositionOffer<double>();immutableOfferConsumption();configurationIdentityHelpers();scalarOnlyRequests();pinnedCandidateWithMovingScalars();latencyAndSafety();check(allocations==0,"zero C++ callback allocations");check(deallocations==0,"zero C++ callback deallocations");std::printf("{\"checks\":%d,\"failures\":%d,\"callback_allocations\":%llu,\"callback_deallocations\":%llu,\"compared_samples\":%llu}\n",checks,failures,static_cast<unsigned long long>(allocations),static_cast<unsigned long long>(deallocations),static_cast<unsigned long long>(comparedSamples));return failures?1:0;} catch(const std::exception &error){std::fprintf(stderr,"Focus exception: %s\n",error.what());return 2;} catch(...){std::fprintf(stderr,"Focus unknown exception\n");return 2;}}
