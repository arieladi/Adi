// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Core.hpp"
#include "Filters.hpp"
#include "Mailbox.hpp"
#include "Transform.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <utility>
#include <vector>

namespace adi::mix::focus {
namespace {
using Complex = std::complex<double>;
constexpr double kKernelRate=44100.;
std::atomic<std::uint64_t> nextPreparationEpoch{1};
bool within(double x,double lo,double hi) noexcept { return std::isfinite(x)&&x>=lo&&x<=hi; }
bool phaseActive(const Settings &s) noexcept {
    return (s.phaseMode==PhaseMode::Match && s.matchAmount>0.) ||
        (s.phaseMode==PhaseMode::Rotation && s.rotationDegrees!=0. && std::abs(s.rotationDegrees)!=180.);
}
// Dry, zero-latency inversion, or an FFT phase path. Both inversion signs
// represent the same prepared topology; zero and inversion remain distinct.
unsigned phaseTopology(const Settings &s) noexcept {
    if(phaseActive(s))return 2U;
    return s.phaseMode==PhaseMode::Rotation&&std::abs(s.rotationDegrees)==180.?1U:0U;
}
std::size_t hopFor(double rate) noexcept { return rate<=48000.?8192:rate<=96000.?16384:32768; }
std::size_t phaseSizeFor(double rate) noexcept { return rate<=48000.?1024:rate<=96000.?2048:4096; }
std::int32_t latencyFor(const Settings &s,double rate) noexcept {
    return static_cast<std::int32_t>((s.engine==Engine::Fir ? hopFor(rate)*3/2 : 0)+
        (phaseActive(s)?phaseSizeFor(rate):0));
}
struct Delay {
    std::array<std::vector<double>,2> samples;
    std::size_t position=0;
    void prepare(std::size_t size,double initial=0.) { for(auto &v:samples) v.assign(size,initial); position=0; }
    void clear(double initial=0.) noexcept {for(auto &v:samples)std::fill(v.begin(),v.end(),initial);position=0;}
    std::array<double,2> tick(const std::array<double,2> &input) noexcept {
        if(samples[0].empty()) return input;
        const std::array<double,2> result{samples[0][position],samples[1][position]};
        samples[0][position]=input[0];samples[1][position]=input[1];
        position=(position+1)%samples[0].size(); return result;
    }
};
struct KeyGuard {
    std::int32_t quiet=0,limit=1;
    bool observe(double key,bool present) noexcept {
        if(!present) { quiet=limit; return false; }
        if(std::abs(key)>1.e-12) { quiet=0; return true; }
        if(quiet<limit) ++quiet;
        return quiet<limit;
    }
};
struct IirCoefficients {
    detail::Biquad detector,solo;
    double alpha=0.,negativeTwoCosine=0.;
};
struct Node {
    detail::Biquad key[2],main[2],audio[2],solo[2];
    detail::Envelope envelope;
    double frequency=-1.,q=-1.,alpha=0.,negativeTwoCosine=0.;
    void configure(const IirCoefficients &coefficients,double f,double quality) noexcept {
        if(frequency==f && q==quality) return;
        frequency=f;q=quality;alpha=coefficients.alpha;negativeTwoCosine=coefficients.negativeTwoCosine;
        for(int i=0;i<2;++i) {
            key[i].coefficientsFrom(coefficients.detector);
            main[i].coefficientsFrom(coefficients.detector);
            solo[i].coefficientsFrom(coefficients.solo);
        }
    }
};
struct Coefficients {
    Settings settings;
    std::uint64_t version=0;
    bool kernelsValid=false;
    std::array<IirCoefficients,kNodes> iir;
    // One coherent extraction kernel per node serves both independent streams.
    std::array<std::vector<Complex>,kNodes> main;
};
struct Slot { std::atomic<int> readers{0}; Coefficients coefficients; };
void kernel(detail::Transform &small,detail::Transform &large,std::vector<double> &time,
            std::vector<Complex> &spectrum,std::vector<double> &window,
            double frequency,double q,std::vector<Complex> &output) {
    const auto h=small.size();
    const auto centre=static_cast<std::size_t>(std::floor(static_cast<double>(h)*frequency/kKernelRate+.5));
    const auto width=2*static_cast<std::size_t>(std::floor(((.9/.1803)*static_cast<double>(centre)/q)/2.))+1;
    window.resize(width);
    std::size_t peak=0;
    for(std::size_t i=0;i<width;++i) {
        const double phase=static_cast<double>(static_cast<float>(std::numbers::pi))*
            std::log10(.1+.9*static_cast<double>(i)/static_cast<double>(width));
        window[i]=.35875-.48829*std::cos(2.*phase)+.14128*std::cos(4.*phase)-.01168*std::cos(6.*phase);
        if(window[i]>window[peak]) peak=i;
    }
    window[peak]=1.;
    std::fill_n(spectrum.begin(),h/2+1,Complex{.001,0.});
    const auto first=centre>peak?centre-peak:0, offset=peak>centre?peak-centre:0;
    if(first<h/2 && offset<width) {
        const auto count=std::min(width-offset,h/2-first);
        for(std::size_t i=0;i<count;++i) spectrum[first+i]=.001+.999*window[offset+i];
    }
    small.inverse(std::span<const Complex>(spectrum.data(),h/2+1),std::span<double>(time.data(),h));
    std::rotate(time.begin(),time.begin()+static_cast<std::ptrdiff_t>(h/2),time.begin()+static_cast<std::ptrdiff_t>(h));
    std::fill(time.begin()+static_cast<std::ptrdiff_t>(h),time.end(),0.);
    large.forward(time,output);
}
} // namespace
bool sameConfiguration(const Settings &a,const Settings &b) noexcept {
    if(a.engine!=b.engine||a.phaseMode!=b.phaseMode||phaseTopology(a)!=phaseTopology(b))return false;
    for(std::size_t i=0;i<kNodes;++i)
        if(a.nodes[i].frequencyHz!=b.nodes[i].frequencyHz||a.nodes[i].q!=b.nodes[i].q)return false;
    return true;
}
Settings pinConfiguration(const Settings &controls,const Settings &plan) noexcept {
    auto result=controls;result.engine=plan.engine;
    for(std::size_t i=0;i<kNodes;++i) {
        result.nodes[i].frequencyHz=plan.nodes[i].frequencyHz;
        result.nodes[i].q=plan.nodes[i].q;
    }
    const bool pinPhase=controls.phaseMode!=plan.phaseMode||phaseTopology(controls)!=phaseTopology(plan);
    result.phaseMode=plan.phaseMode;
    if(pinPhase) {result.matchAmount=plan.matchAmount;result.rotationDegrees=plan.rotationDegrees;}
    return result;
}
bool valid(const Settings &s) noexcept {
    if(static_cast<unsigned>(s.engine)>static_cast<unsigned>(Engine::Fir) ||
       static_cast<unsigned>(s.detector)>static_cast<unsigned>(Detector::Difference) ||
       static_cast<unsigned>(s.phaseMode)>static_cast<unsigned>(PhaseMode::Rotation) ||
       !within(s.matchAmount,0.,1.) || !within(s.rotationDegrees,-180.,180.) ||
       !within(s.phaseMinimumHz,0.,20000.) || !within(s.phaseMaximumHz,0.,20000.) ||
       !within(s.wet,0.,1.) || !within(s.inputGainDb,-48.,48.) || !within(s.outputGainDb,-48.,48.) ||
       !within(s.keyCalibrationDb,-48.,48.) || !within(s.inputCalibrationDb,-48.,48.)) return false;
    for(const auto &n:s.nodes) if(!within(n.frequencyHz,10.,20000.) || !within(n.q,.1,5.) ||
        !within(n.thresholdDb,-90.,0.) || !within(n.differenceDb,-10.,0.) ||
        !within(n.attackMs,1.,50.) || !within(n.releaseMs,25.,250.) || !within(n.position,0.,1.)) return false;
    return true;
}
struct Core::Impl {
    PrepareSpec spec;
    bool prepared=false;
    Settings lastRequested;
    detail::ConfigurationMailbox mailbox;
    std::array<Slot,3> slots;
    std::atomic<int> published{0};
    int activeSlot=0;
    std::uint64_t generation=0,physical=0,serial=0,epoch=0;
    std::atomic<std::uint64_t> ready{0},applied{0},pendingToken{0};
    std::atomic<std::int32_t> activeLatency{0},pendingSamples{0};
    Statistics stats;
    detail::Transform builderSmall,builderLarge,firTransform,phaseTransform;
    std::vector<double> buildTime,buildWindow;
    std::vector<Complex> buildSpectrum;
    std::size_t h=0,n=0,phaseN=0,phaseH=0,firFill=0;
    std::uint64_t firCursor=0,phaseCursor=0;
    std::array<std::array<Node,kNodes>,2> iir;
    std::array<std::array<detail::Envelope,kNodes>,2> firEnvelope;
    std::array<std::vector<double>,2> firInput,firKey,phaseInput,phaseKey,phaseOutput;
    std::array<std::vector<Complex>,2> firMainSpectrum,firKeySpectrum;
    std::array<std::array<std::vector<double>,kNodes>,2> bandMain,bandKey;
    std::vector<double> transformTime,phaseTime,phaseWindow;
    std::vector<Complex> transformSpectrum,phaseSpectrum,phaseKeySpectrum;
    Delay firDry,phaseDry,phaseKeyDelay,phaseValidityDelay;
    std::array<KeyGuard,2> keyGuard;
    std::array<double,2> phaseBlend{1.,1.};
    double phaseBlendStep=1.;
    void build(Coefficients &destination,const Settings &s,std::uint64_t version,const Coefficients *previous) {
        destination.settings=s;destination.version=version;
        destination.kernelsValid=false;
        if(s.engine!=Engine::Fir) {
            for(std::size_t b=0;b<kNodes;++b) {
                auto &c=destination.iir[b];const auto &node=s.nodes[b];
                const double omega=2.*std::numbers::pi*node.frequencyHz/spec.sampleRate;
                c.alpha=std::sin(omega)/(2.*node.q);c.negativeTwoCosine=-2.*std::cos(omega);
                c.detector.bandpass(omega,node.frequencyHz<=200.?node.frequencyHz*.01:node.q);
                c.solo.bandpass(omega,node.q);
            }
            return;
        }
        for(std::size_t b=0;b<kNodes;++b) {
            // Frequency and Q jointly identify one complete main/key kernel.
            // Identical settings never depend on setter order or preset history.
            const bool same=previous && previous->kernelsValid && previous->settings.nodes[b].frequencyHz==s.nodes[b].frequencyHz;
            if(same && previous->settings.nodes[b].q==s.nodes[b].q) {
                destination.main[b]=previous->main[b];
                continue;
            }
            kernel(builderSmall,builderLarge,buildTime,buildSpectrum,buildWindow,
                s.nodes[b].frequencyHz,s.nodes[b].q,destination.main[b]);
        }
        destination.kernelsValid=true;
    }
    void prepare(PrepareSpec next,const Settings &s) {
        if(!valid(s)||!std::isfinite(next.sampleRate)||next.sampleRate<44100.||next.sampleRate>192000.||
           next.maximumFrames<1||next.maximumFrames>(1<<20)||next.channels!=2)
            throw std::invalid_argument("Focus prepare requires stereo main, 44.1-192 kHz and a positive block capacity");
        prepared=false;spec=next;h=hopFor(spec.sampleRate);n=2*h;
        phaseN=phaseSizeFor(spec.sampleRate);phaseH=phaseN/4;
        builderSmall.prepare(h);builderLarge.prepare(n);firTransform.prepare(n);phaseTransform.prepare(phaseN);
        buildTime.assign(n,0.);buildSpectrum.resize(h+1);buildWindow.clear();
        transformTime.assign(n,0.);transformSpectrum.resize(h+1);
        phaseTime.assign(phaseN,0.);phaseSpectrum.resize(phaseN/2+1);phaseKeySpectrum.resize(phaseN/2+1);
        phaseWindow.resize(phaseN);
        for(std::size_t i=0;i<phaseN;++i) {
            const float angle=(static_cast<float>(i)*static_cast<float>(2.*std::numbers::pi))/static_cast<float>(phaseN-1);
            phaseWindow[i]=static_cast<double>(1.f-std::cos(angle))*.5;
        }
        for(std::size_t c=0;c<2;++c) {
            firInput[c].assign(h,0.);firKey[c].assign(h,0.);
            firMainSpectrum[c].resize(h+1);firKeySpectrum[c].resize(h+1);
            phaseInput[c].assign(phaseN,0.);phaseKey[c].assign(phaseN,0.);phaseOutput[c].assign(4*phaseN,0.);
            for(std::size_t b=0;b<kNodes;++b) {
                bandMain[c][b].assign(4*h,0.);bandKey[c][b].assign(4*h,0.);
                iir[c][b]=Node{};firEnvelope[c][b]=detail::Envelope{};
            }
            keyGuard[c]={0,std::max(1,static_cast<int>(spec.sampleRate*.01))};
        }
        for(auto &slot:slots) {
            slot.readers.store(0,std::memory_order_relaxed);
            for(auto &v:slot.coefficients.main) v.resize(h+1);
        }
        lastRequested=s;mailbox.reset();
        const auto version=mailbox.publish(s);
        build(slots[0].coefficients,s,version,nullptr);
        activeSlot=0;slots[0].readers.store(1,std::memory_order_relaxed);published.store(0,std::memory_order_release);
        ready.store(version,std::memory_order_release);applied.store(version,std::memory_order_release);
        pendingToken.store(0,std::memory_order_release);pendingSamples.store(latencyFor(s,spec.sampleRate),std::memory_order_release);
        activeLatency.store(latencyFor(s,spec.sampleRate),std::memory_order_release);
        firDry.prepare(h*3/2);phaseDry.prepare(phaseN);phaseKeyDelay.prepare(phaseN);phaseValidityDelay.prepare(phaseN,1.);
        firFill=0;firCursor=phaseCursor=0;physical=serial=0;++generation;
        epoch=nextPreparationEpoch.fetch_add(1,std::memory_order_relaxed);stats={};phaseBlend={1.,1.};
        phaseBlendStep=1./std::max(1.,spec.sampleRate*.01);prepared=true;
    }
    // Owner-only, while the relevant bank/stage is dormant. Re-entering after
    // another path has consumed source time cannot replay its old delayed audio.
    void clearFirSignal() noexcept {
        for(std::size_t c=0;c<2;++c) {
            std::fill(firInput[c].begin(),firInput[c].end(),0.);
            std::fill(firKey[c].begin(),firKey[c].end(),0.);
            for(std::size_t b=0;b<kNodes;++b) {
                std::fill(bandMain[c][b].begin(),bandMain[c][b].end(),0.);
                std::fill(bandKey[c][b].begin(),bandKey[c][b].end(),0.);
                firEnvelope[c][b]={};
            }
        }
        firDry.clear();firFill=0;firCursor=0;
    }
    void clearIirSignal() noexcept {
        for(auto &channel:iir)for(auto &node:channel)node=Node{};
    }
    void clearPhaseSignal() noexcept {
        for(std::size_t c=0;c<2;++c) {
            std::fill(phaseInput[c].begin(),phaseInput[c].end(),0.);
            std::fill(phaseKey[c].begin(),phaseKey[c].end(),0.);
            std::fill(phaseOutput[c].begin(),phaseOutput[c].end(),0.);
        }
        phaseDry.clear();phaseKeyDelay.clear();phaseValidityDelay.clear(1.);
        phaseCursor=0;phaseBlend={1.,1.};
    }
    bool request(const Settings &s) noexcept {
        if(!prepared||!valid(s)) return false;
        if(sameConfiguration(s,lastRequested)) {lastRequested=s;return true;}
        if(mailbox.publish(s)==0) return false;
        lastRequested=s;return true;
    }
    bool service() {
        if(!prepared || pendingToken.load(std::memory_order_acquire)!=0) return false;
        Settings s;std::uint64_t version=0;
        if(!mailbox.read(s,version)||version==ready.load(std::memory_order_acquire)) return false;
        // This owner's previous nonzero token can become zero only through
        // acquire-observed audio admission. Thus published is the immutable
        // admitted plan, and any other bank/off-phase storage is dormant.
        const int current=published.load(std::memory_order_acquire);
        for(int index=0;index<3;++index) {
            if(index==current) continue;
            int zero=0;
            if(!slots[static_cast<std::size_t>(index)].readers.compare_exchange_strong(zero,-1,std::memory_order_acq_rel)) continue;
            auto &slot=slots[static_cast<std::size_t>(index)];
            const auto &previous=slots[static_cast<std::size_t>(current)].coefficients;
            if(previous.settings.engine!=s.engine) {
                if(s.engine==Engine::Fir)clearFirSignal();else clearIirSignal();
            }
            if(!phaseActive(previous.settings)&&phaseActive(s))clearPhaseSignal();
            try { build(slot.coefficients,s,version,&previous); }
            catch(...) { slot.readers.store(0,std::memory_order_release);throw; }
            slot.readers.store(0,std::memory_order_release);
            published.store(index,std::memory_order_release);ready.store(version,std::memory_order_release);
            pendingSamples.store(latencyFor(s,spec.sampleRate),std::memory_order_release);
            pendingToken.store(version,std::memory_order_release);return true;
        }
        return false;
    }
    BlockContext begin(std::uint64_t block,std::int32_t configured,const Settings &s,bool admitPending) noexcept {
        if(!prepared||!valid(s)) return {};
        // A delayed control publication cannot invalidate a usable audio plan.
        // lastRequested advances only on success, so a later call retries it.
        static_cast<void>(request(s));
        if(block!=0&&block==physical)
            return configured==activeLatency.load(std::memory_order_acquire)?BlockContext{generation,serial,epoch}:BlockContext{};
        physical=block;++serial;
        const int next=published.load(std::memory_order_acquire);
        if(admitPending&&next!=activeSlot) {
            auto &slot=slots[static_cast<std::size_t>(next)];int readers=slot.readers.load(std::memory_order_relaxed);
            bool held=false;
            for(int attempt=0;attempt<3&&readers>=0;++attempt) {
                if(slot.readers.compare_exchange_strong(readers,readers+1,std::memory_order_acquire)) { held=true;break; }
            }
            if(held) {
                const auto &plan=slot.coefficients;
                if(plan.version>applied.load(std::memory_order_relaxed)&&
                   pendingToken.load(std::memory_order_acquire)==plan.version&&
                   latencyFor(plan.settings,spec.sampleRate)==configured) {
                    slots[static_cast<std::size_t>(activeSlot)].readers.fetch_sub(1,std::memory_order_release);
                    activeSlot=next;applied.store(plan.version,std::memory_order_release);++generation;
                    activeLatency.store(configured,std::memory_order_release);
                    if(pendingToken.load(std::memory_order_acquire)==plan.version) pendingToken.store(0,std::memory_order_release);
                } else slot.readers.fetch_sub(1,std::memory_order_release);
            }
        }
        if(configured!=activeLatency.load(std::memory_order_acquire))return {};
        return {generation,serial,epoch};
    }
    void phaseHop(const Settings &s) noexcept {
        const auto oldest=static_cast<std::size_t>((phaseCursor+1)%phaseN);
        const auto low=std::min(phaseN/2,static_cast<std::size_t>(static_cast<double>(phaseN-1)*s.phaseMinimumHz/kKernelRate));
        for(std::size_t c=0;c<2;++c) {
            for(std::size_t i=0;i<phaseN;++i) phaseTime[i]=phaseInput[c][(oldest+i)%phaseN]*phaseWindow[i];
            phaseTransform.forward(phaseTime,phaseSpectrum);
            if(s.phaseMode==PhaseMode::Match) {
                for(std::size_t i=0;i<phaseN;++i) phaseTime[i]=phaseKey[c][(oldest+i)%phaseN]*phaseWindow[i];
                phaseTransform.forward(phaseTime,phaseKeySpectrum);
            }
            for(std::size_t i=0;i<phaseN/2;++i) {
                if((i==0&&low==0)||(i>0&&i<low)) continue;
                const auto z=phaseSpectrum[i];const double mainAngle=std::atan2(z.imag(),z.real());
                double angle=mainAngle+s.rotationDegrees*std::numbers::pi/180.;
                if(s.phaseMode==PhaseMode::Match) {
                    const auto k=phaseKeySpectrum[i];
                    const double keyAngle=k.real()==0.&&k.imag()==0.?0.:std::atan2(k.imag(),k.real());
                    angle=s.matchAmount*keyAngle+(1.-s.matchAmount)*mainAngle;
                }
                phaseSpectrum[i]=std::polar(std::abs(z),angle);
                if(i==0) phaseSpectrum[i]={phaseSpectrum[i].real(),0.};
            }
            phaseTransform.inverse(phaseSpectrum,phaseTime);
            for(std::size_t i=0;i<phaseN;++i)
                phaseOutput[c][static_cast<std::size_t>((phaseCursor+1+i)%(4*phaseN))]+=phaseTime[i]*phaseWindow[i]/1.5;
        }
        ++stats.phaseHops;
    }
    std::array<double,2> phaseTick(std::array<double,2> main,std::array<double,2> key,
        const Settings &s,const std::array<bool,2> &validKey) noexcept {
        const auto dry=phaseDry.tick(main);
        std::array<double,2> result{};
        const auto outIndex=static_cast<std::size_t>(phaseCursor%(4*phaseN));
        const auto inIndex=static_cast<std::size_t>(phaseCursor%phaseN);
        for(std::size_t c=0;c<2;++c) {
            const double transformed=phaseOutput[c][outIndex];phaseOutput[c][outIndex]=0.;
            phaseInput[c][inIndex]=main[c];phaseKey[c][inIndex]=key[c];
            const double target=s.phaseMode==PhaseMode::Match&&!validKey[c]?0.:1.;
            phaseBlend[c]+=std::clamp(target-phaseBlend[c],-phaseBlendStep,phaseBlendStep);
            result[c]=phaseBlend[c]*transformed+(1.-phaseBlend[c])*dry[c];
        }
        if((phaseCursor+1)%phaseH==0) phaseHop(s);
        ++phaseCursor;return result;
    }
    void firHop(const Coefficients &plan,const Settings &s) noexcept {
        for(std::size_t c=0;c<2;++c) {
            std::copy(firInput[c].begin(),firInput[c].end(),transformTime.begin());
            std::fill(transformTime.begin()+static_cast<std::ptrdiff_t>(h),transformTime.end(),0.);
            firTransform.forward(transformTime,firMainSpectrum[c]);
            std::copy(firKey[c].begin(),firKey[c].end(),transformTime.begin());
            firTransform.forward(transformTime,firKeySpectrum[c]);
            for(std::size_t b=0;b<kNodes;++b) if(s.nodes[b].enabled) {
                for(std::size_t k=0;k<=h;++k) transformSpectrum[k]=firMainSpectrum[c][k]*plan.main[b][k];
                firTransform.inverse(transformSpectrum,transformTime);
                for(std::size_t k=0;k<n-1;++k)
                    bandMain[c][b][static_cast<std::size_t>((firCursor+1+k)%(4*h))]+=transformTime[k];
                for(std::size_t k=0;k<=h;++k) transformSpectrum[k]=firKeySpectrum[c][k]*plan.main[b][k];
                firTransform.inverse(transformSpectrum,transformTime);
                for(std::size_t k=0;k<n-1;++k)
                    bandKey[c][b][static_cast<std::size_t>((firCursor+1+k)%(4*h))]+=transformTime[k];
            }
        }
        ++stats.filterHops;
    }
    template<class Sample> bool process(Sample *const *main,const Sample *const *key,std::int32_t keyChannels,
        std::int32_t frames,const Settings &s,BlockContext context) noexcept {
        if(!prepared||!valid(s)||context.epoch!=epoch||context.generation!=generation||context.block!=serial||frames<0||
           !main||!main[0]||!main[1]||keyChannels<0||keyChannels>2||
           (keyChannels>0&&(!key||!key[0]||(keyChannels==2&&!key[1])))) return false;
        request(s);
        const auto &plan=slots[static_cast<std::size_t>(activeSlot)].coefficients;
        const auto &fixed=plan.settings;
        const auto phaseSettings=pinConfiguration(s,fixed);
        const bool runPhase=phaseActive(fixed),fir=fixed.engine==Engine::Fir;
        const double inGain=std::pow(10.,(s.inputGainDb+s.inputCalibrationDb)*.05);
        const double outGain=std::pow(10.,(s.outputGainDb-s.inputCalibrationDb)*.05);
        const double keyGain=std::pow(10.,s.keyCalibrationDb*.05);
        const bool difference=s.detector==Detector::Difference;
        const double rate=fir?kKernelRate:spec.sampleRate;
        bool anyEnabled=false;
        for(const auto &node:s.nodes) anyEnabled=anyEnabled||node.enabled;
        if(!runPhase&&!fir&&!anyEnabled) {
            const double gain=inGain*outGain;
            const double sign=fixed.phaseMode==PhaseMode::Rotation&&std::abs(fixed.rotationDegrees)==180.?-1.:1.;
            if(gain!=1.||sign!=1.) for(std::int32_t frame=0;frame<frames;++frame)
                for(std::size_t c=0;c<2;++c) main[c][frame]=static_cast<Sample>(static_cast<double>(main[c][frame])*gain*sign);
            stats.processedFrames+=static_cast<std::uint64_t>(frames);return true;
        }
        std::array<double,kNodes> thresholds{},attacks{},releases{},midWeight{},sideWeight{};
        bool anySolo=false;
        for(std::size_t b=0;b<kNodes;++b) {
            const auto &node=s.nodes[b];
            const double f=plan.settings.nodes[b].frequencyHz;
            const double calibration=difference?0.:(fir?
                17.56*std::exp(3.112e-5*f)-16.77*std::exp(-.01135*f):
                15.33*std::exp(-6.118e-5*f)-9.135*std::exp(-.003684*f));
            thresholds[b]=std::min(0.,(difference?node.differenceDb:node.thresholdDb)+calibration);
            attacks[b]=std::exp(-1./(rate*.001*node.attackMs));releases[b]=std::exp(-1./(rate*.001*node.releaseMs));
            midWeight[b]=std::min(1.,2.*(1.-node.position));sideWeight[b]=std::min(1.,2.*node.position);
            anySolo=anySolo||(node.enabled&&node.solo&&!node.bypass);
            if(!fir) for(std::size_t c=0;c<2;++c)
                iir[c][b].configure(plan.iir[b],plan.settings.nodes[b].frequencyHz,plan.settings.nodes[b].q);
        }
        for(std::int32_t frame=0;frame<frames;++frame) {
            const double left=static_cast<double>(main[0][frame])*inGain,right=static_cast<double>(main[1][frame])*inGain;
            std::array<double,2> x{left+right,left-right},k{};
            const bool present=!s.externalKey||keyChannels>0;
            if(!s.externalKey) k=x;
            else if(keyChannels==1) k={static_cast<double>(key[0][frame])*keyGain,static_cast<double>(key[0][frame])*keyGain};
            else if(keyChannels==2) {
                const double kl=static_cast<double>(key[0][frame])*keyGain,kr=static_cast<double>(key[1][frame])*keyGain;
                k={kl+kr,kl-kr};
            }
            std::array<bool,2> keyValid{keyGuard[0].observe(k[0],present),keyGuard[1].observe(k[1],present)};
            if(runPhase) {
                const auto heardValid=phaseValidityDelay.tick({keyValid[0]?1.:0.,keyValid[1]?1.:0.});
                keyValid={heardValid[0]!=0.,heardValid[1]!=0.};
                x=phaseTick(x,k,phaseSettings,keyValid);k=phaseKeyDelay.tick(k);
            }
            else if(fixed.phaseMode==PhaseMode::Rotation&&std::abs(fixed.rotationDegrees)==180.) {x[0]=-x[0];x[1]=-x[1];}
            std::array<double,2> y=x;
            if(fir) {
                const auto dry=firDry.tick(x);y=anySolo?std::array<double,2>{0.,0.}:dry;
                const auto index=static_cast<std::size_t>(firCursor%(4*h));
                for(std::size_t c=0;c<2;++c) {
                    firInput[c][firFill]=x[c];firKey[c][firFill]=k[c];
                    for(std::size_t b=0;b<kNodes;++b) {
                        const double band=bandMain[c][b][index],bandK=bandKey[c][b][index];
                        bandMain[c][b][index]=0.;bandKey[c][b][index]=0.;
                        const auto &node=s.nodes[b];if(!node.enabled) continue;
                        const double weight=c==0?midWeight[b]:sideWeight[b];
                        const bool detectorKeyValid=present&&(keyValid[c]||std::abs(bandK)>1.e-12||firEnvelope[c][b].rms>1.e-12);
                        const double gain=firEnvelope[c][b].tick(band,bandK,thresholds[b],attacks[b],releases[b],difference,detectorKeyValid);
                        const double appliedGain=std::min(0.,weight*gain);
                        if(!node.bypass&&(!anySolo||node.solo))
                            y[c]-=(1.-std::pow(10.,appliedGain*.05))*band*s.wet;
                    }
                }
                if(++firFill==h) { firHop(plan,s);firFill=0; }
                ++firCursor;
            } else {
                for(std::size_t c=0;c<2;++c) for(std::size_t b=0;b<kNodes;++b) {
                    const auto &node=s.nodes[b];if(!node.enabled) continue;
                    auto &state=iir[c][b];
                    const double filteredKey=state.key[1].tick(state.key[0].tick(k[c]));
                    const double filteredMain=difference?state.main[1].tick(state.main[0].tick(x[c])):0.;
                    const bool detectorKeyValid=present&&(keyValid[c]||std::abs(filteredKey)>1.e-12||state.envelope.rms>1.e-12);
                    const double gain=state.envelope.tick(filteredMain,filteredKey,thresholds[b],attacks[b],releases[b],difference,detectorKeyValid);
                    const double appliedGain=std::min(0.,gain*(c==0?midWeight[b]:sideWeight[b]));
                    if(node.bypass||(anySolo&&!node.solo)) continue;
                    if(node.solo) y[c]=state.solo[1].tick(state.solo[0].tick(y[c]))*(1.-std::pow(10.,appliedGain*.05));
                    else {
                        state.audio[0].peak(state.alpha,state.negativeTwoCosine,appliedGain*s.wet);
                        state.audio[1].coefficientsFrom(state.audio[0]);
                        y[c]=state.audio[1].tick(state.audio[0].tick(y[c]));
                    }
                }
            }
            main[0][frame]=static_cast<Sample>((y[0]+y[1])*.5*outGain);
            main[1][frame]=static_cast<Sample>((y[0]-y[1])*.5*outGain);
        }
        stats.processedFrames+=static_cast<std::uint64_t>(frames);return true;
    }
};
Core::Core():impl_(std::make_unique<Impl>()){}
Core::~Core()=default;
void Core::prepare(PrepareSpec spec,const Settings &s){impl_->prepare(spec,s);}
void Core::reset(const Settings &s){impl_->prepare(impl_->spec,s);}
bool Core::requestConfiguration(const Settings &s) noexcept{return impl_->request(s);}
bool Core::serviceConfiguration(){return impl_->service();}
ConfigurationStatus Core::configurationStatus() const noexcept {
    return {impl_->mailbox.sequence.load(std::memory_order_acquire),impl_->ready.load(std::memory_order_acquire),impl_->applied.load(std::memory_order_acquire)};
}
LatencyChange Core::pendingLatencyChange() const noexcept {
    for(int attempt=0;attempt<3;++attempt) {
        const auto token=impl_->pendingToken.load(std::memory_order_acquire);
        if(token==0) return {};
        const auto samples=impl_->pendingSamples.load(std::memory_order_acquire);
        if(impl_->pendingToken.load(std::memory_order_acquire)==token) return {token,samples};
    }
    return {};
}
std::int32_t Core::latencySamples() const noexcept{return impl_->activeLatency.load(std::memory_order_acquire);}
BlockContext Core::beginBlock(std::uint64_t physical,std::int32_t latency,const Settings &s,bool admitPending) noexcept{return impl_->begin(physical,latency,s,admitPending);}
bool Core::process(float *const *main,const float *const *key,std::int32_t channels,std::int32_t frames,const Settings &s,BlockContext context) noexcept{return impl_->process(main,key,channels,frames,s,context);}
bool Core::process(double *const *main,const double *const *key,std::int32_t channels,std::int32_t frames,const Settings &s,BlockContext context) noexcept{return impl_->process(main,key,channels,frames,s,context);}
Statistics Core::statistics() const noexcept{return impl_->stats;}
} // namespace adi::mix::focus
