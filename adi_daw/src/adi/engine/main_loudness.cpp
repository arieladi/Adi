// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/main_loudness.hpp"
#include "adi/engine/publisher.hpp"
#include "adi/dsp/auto_gain.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>
namespace adi::engine {
namespace {
constexpr float silence=-std::numeric_limits<float>::infinity();
double loudness(double energy) noexcept { return energy>0 ? -0.691+10*std::log10(energy) : -std::numeric_limits<double>::infinity(); }
float dbPeak(double peak) noexcept { return peak>0 ? static_cast<float>(20*std::log10(peak)) : silence; }
struct Filter {
    dsp::Biquad c;
    double x1=0,x2=0,y1=0,y2=0;
    double run(double x) noexcept {
        const double y=c.b0*x+c.b1*x1+c.b2*x2-c.a1*y1-c.a2*y2;
        x2=x1;x1=x;y2=y1;y1=std::abs(y)<1e-30 ? 0 : y;return y1;
    }
};
// The same Kaiser-sinc design as dsp::TruePeakMeter (BS.1770 Annex 2),
// evaluated in double for both graph widths. Design only on the message thread.
double bessel(double x) {
    double sum=1,term=1;
    for(int k=1;k<40;++k){const double v=x/(2*k);term*=v*v;sum+=term;if(term<1e-15*sum)break;}
    return sum;
}
struct Channel {
    Filter shelf,hp;
    std::array<double,16> history{};
    double weight=1;
};
struct Bin { double energy=0;std::uint64_t count=0; };
struct Dsp {
    // Fixed 0.1 LU bins, like libebur128's bounded histogram option. Store
    // actual energy, not bin centres. Relative-gate quantisation is <=0.1 LU;
    // memory does not grow with running time and no callback appends a vector.
    static constexpr std::size_t binCount=22001;
    std::array<Bin,binCount> histogram{};
    std::array<double,30> hops{};
    std::array<double,64> taps{};
    std::vector<Channel> channels;
    int factor=4,head=0,hopFrames=4800,hopAt=0;
    std::size_t nextHop=0,complete=0;
    double hopEnergy=0,peak=0;
    explicit Dsp(double rate,const std::vector<double>& weights):channels(weights.size()) {
        factor=rate<88199 ? 4 : rate<176399 ? 2 : 1;
        hopFrames=static_cast<int>(std::lround(rate*0.1));
        for(std::size_t c=0;c<channels.size();++c){channels[c].shelf.c=dsp::AutoGain::kShelf(rate);channels[c].hp.c=dsp::AutoGain::kHighPass(rate);channels[c].weight=weights[c];}
        if(factor==1)return;
        const int n=factor*16;const double centre=(n-1)*0.5,norm=bessel(8);
        for(int i=0;i<n;++i) {
            const double t=(i-centre)/factor,r=(i-centre)/(n*0.5);
            const double sinc=t==0 ? 1 : std::sin(std::numbers::pi*t)/(std::numbers::pi*t);
            taps[static_cast<std::size_t>(i)]=sinc*bessel(8*std::sqrt(std::max(0.0,1-r*r)))/norm;
        }
        for(int p=0;p<factor;++p) {
            double sum=0;for(int k=0;k<16;++k)sum+=taps[static_cast<std::size_t>(p+k*factor)];
            for(int k=0;k<16;++k)taps[static_cast<std::size_t>(p+k*factor)]/=sum;
        }
    }
    void reset() noexcept {
        histogram.fill({});hops.fill(0);head=hopAt=0;nextHop=complete=0;hopEnergy=peak=0;
        for(auto& c:channels){c.shelf.x1=c.shelf.x2=c.shelf.y1=c.shelf.y2=0;c.hp.x1=c.hp.x2=c.hp.y1=c.hp.y2=0;c.history.fill(0);}
    }
    void publishWindow(MainLoudnessReadout& readout) noexcept {
        hops[nextHop]=hopEnergy/hopFrames;nextHop=(nextHop+1)%hops.size();
        complete=std::min(complete+1,hops.size());hopEnergy=0;hopAt=0;
        double momentary=0,shortTerm=0;
        for(std::size_t k=0;k<complete;++k) {
            const auto energy=hops[(nextHop+hops.size()-1-k)%hops.size()];
            shortTerm+=energy;if(k<4)momentary+=energy;
        }
        readout.momentaryLufs.store(complete>=4 ? static_cast<float>(loudness(momentary/4)) : silence,std::memory_order_relaxed);
        readout.shortTermLufs.store(complete==30 ? static_cast<float>(loudness(shortTerm/30)) : silence,std::memory_order_relaxed);
        if(complete<4)return;
        const double energy=momentary/4,level=loudness(energy);
        if(level>=-70) {
            const auto index=static_cast<std::size_t>(std::clamp(std::floor((level+70)*10),0.0,static_cast<double>(binCount-1)));
            histogram[index].energy+=energy;++histogram[index].count;
        }
        double total=0,count=0;
        for(const auto& bin:histogram){total+=bin.energy;count+=static_cast<double>(bin.count);}
        if(count==0){readout.integratedLufs.store(silence,std::memory_order_relaxed);return;}
        const double relative=total/count*0.1;
        total=count=0;
        for(const auto& bin:histogram)if(bin.count && bin.energy/static_cast<double>(bin.count)>=relative){total+=bin.energy;count+=static_cast<double>(bin.count);}
        readout.integratedLufs.store(count>0 ? static_cast<float>(loudness(total/count)) : silence,std::memory_order_relaxed);
    }
    template<class T> void process(const T* const* output,int count,int frames,MainLoudnessReadout& readout) noexcept {
        for(int i=0;i<frames;++i) {
            double energy=0;
            for(std::size_t c=0;c<channels.size();++c) {
                auto& channel=channels[c];
                const double raw=static_cast<int>(c)<count && output && output[c] ? static_cast<double>(output[c][i]) : 0;
                // Protect only meter state from a bad device. The actual audio
                // is untouched; this range keeps even squared wide input finite.
                const double sample=std::isfinite(raw) ? std::clamp(raw,-1e100,1e100) : 0;
                const double weighted=channel.hp.run(channel.shelf.run(sample));energy+=weighted*weighted*channel.weight;
                peak=std::max(peak,std::abs(sample));channel.history[static_cast<std::size_t>(head)]=sample;
                if(factor>1)for(int p=0;p<factor;++p) {
                    double value=0;int at=head;
                    for(int k=0;k<16;++k){value+=taps[static_cast<std::size_t>(p+k*factor)]*channel.history[static_cast<std::size_t>(at)];at=at==0?15:at-1;}
                    peak=std::max(peak,std::abs(value));
                }
            }
            head=(head+1)%16;hopEnergy+=energy;
            if(++hopAt==hopFrames)publishWindow(readout);
        }
        readout.truePeakDb.store(dbPeak(peak),std::memory_order_relaxed);
    }
};
struct Format final:Sequenced {
    // The pointer/format are immutable after publication; only the single
    // audio reader mutates the independently owned DSP state.
    std::unique_ptr<Dsp> render;
    Format(double rate,const std::vector<double>& weights):render(std::make_unique<Dsp>(rate,weights)){}
};
void clear(MainLoudnessReadout& r) noexcept {
    r.momentaryLufs.store(silence,std::memory_order_relaxed);r.shortTermLufs.store(silence,std::memory_order_relaxed);
    r.integratedLufs.store(silence,std::memory_order_relaxed);r.truePeakDb.store(silence,std::memory_order_relaxed);
}
}
struct MainLoudness::Impl {
    SnapshotPublisher<Format> formats;
    double rate=0;std::vector<double> weights;
    std::uint64_t playingSeq=0; // audio only
};
MainLoudness::MainLoudness():impl_(std::make_unique<Impl>()){}
MainLoudness::~MainLoudness()=default;
bool MainLoudness::prepare(double sampleRate,int channels,std::span<const double> weights) {
    if(!std::isfinite(sampleRate) || sampleRate<8000 || sampleRate>768000 || channels<1 || channels>64 ||
       (!weights.empty() && weights.size()!=static_cast<std::size_t>(channels)))return false;
    std::vector<double> next(static_cast<std::size_t>(channels),1);
    if(!weights.empty())std::copy(weights.begin(),weights.end(),next.begin());
    for(double weight:next)if(!std::isfinite(weight) || weight<0 || weight>2)return false;
    if(impl_->rate==sampleRate && impl_->weights==next)return true;
    auto format=std::make_unique<Format>(sampleRate,next);
    impl_->formats.publish(std::move(format));impl_->rate=sampleRate;impl_->weights=std::move(next);collect();return true;
}
void MainLoudness::collect(){impl_->formats.collect();}
std::uint64_t MainLoudness::preparedFormatSequence() const noexcept {
    const auto* format=impl_->formats.peek();
    return format ? format->seq : 0;
}
void MainLoudness::process(const AudioIo& io) noexcept {
    SnapshotPublisher<Format>::AudioRead format(impl_->formats);
    if(!format.valid())return;
    if(impl_->playingSeq!=format->seq){
        readout_.formatSequence.store(0,std::memory_order_release);
        impl_->playingSeq=format->seq;clear(readout_);
        readout_.formatSequence.store(format->seq,std::memory_order_release);
    }
    if(reset_.exchange(false,std::memory_order_acq_rel)){format->render->reset();clear(readout_);}
    if(io.frames<=0)return;
    if(io.out64)format->render->process(io.out64,io.numOut,io.frames,readout_);
    else format->render->process(io.out,io.numOut,io.frames,readout_);
}
}
