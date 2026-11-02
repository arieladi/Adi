// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/transients.hpp"
#include "adi/audio/io_audit.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
namespace adi::audio {
TransientResult detectTransients(WavReader& reader,std::uint64_t first,std::uint64_t frames,int sensitivity,std::stop_token stop) {
    TransientResult result;
    if(onAudioThread){result.error=AnalysisError::Realtime;return result;}
    if(first>reader.frames() || frames==0 || frames>reader.frames()-first || reader.sampleRate()<8000 ||
       reader.sampleRate()>768000 || reader.channels()==0 || reader.channels()>64 || sensitivity<0 || sensitivity>100) {
        result.error=AnalysisError::InvalidSource;return result;
    }
    if(stop.stop_requested()){result.error=AnalysisError::Cancelled;return result;}
    const auto window=std::max<std::uint32_t>(1,reader.sampleRate()/200);
    const auto separation=std::max<std::uint64_t>(1,reader.sampleRate()/25); // 40 ms
    const double multiplier=5.0-3.0*sensitivity/100.0;
    std::vector<float> input(static_cast<std::size_t>(window)*reader.channels());
    double previous=0,baseline=0;std::uint64_t last=0;bool emitted=false;
    try {
        reader.seek(first);
        for(std::uint64_t position=0;position<frames;) {
            if(stop.stop_requested()){result.error=AnalysisError::Cancelled;return result;}
            const auto count=static_cast<std::uint32_t>(std::min<std::uint64_t>(window,frames-position));
            if(reader.read(input.data(),count)!=count){result.error=AnalysisError::Io;return result;}
            double square=0,peak=0;
            for(std::size_t index=0;index<static_cast<std::size_t>(count)*reader.channels();++index) {
                const double value=std::isfinite(input[index])?input[index]:0;
                square+=value*value;peak=std::max(peak,std::abs(value));
            }
            const double energy=std::sqrt(square/(static_cast<double>(count)*reader.channels()));
            const double rise=std::max(0.0,energy-previous);
            if(peak>0.0005 && rise>std::max({1e-7,baseline*multiplier,energy*0.3}) &&
               (!emitted || position>=last+separation)) {
                std::uint64_t onset=position;
                for(std::uint32_t frame=0;frame<count;++frame) {
                    bool found=false;
                    for(std::uint16_t channel=0;channel<reader.channels();++channel)
                        if(std::isfinite(input[static_cast<std::size_t>(frame)*reader.channels()+channel]) && std::abs(input[static_cast<std::size_t>(frame)*reader.channels()+channel])>=peak*0.1)found=true;
                    if(found){onset+=frame;break;}
                }
                if(result.frames.size()==1000000){result.error=AnalysisError::TooManyOnsets;return result;}
                result.frames.push_back(onset);last=onset;emitted=true;
            }
            baseline=baseline*0.98+rise*0.02;previous=energy;position+=count;
        }
    } catch(...){result.error=AnalysisError::Io;}
    return result;
}
}
